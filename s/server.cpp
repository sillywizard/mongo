// server.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "../util/message.h"
#include "../util/unittest.h"
#include "../client/connpool.h"
#include "../util/message_server.h"

#include "server.h"
#include "request.h"
#include "config.h"
#include "chunk.h"
#include "balance.h"

namespace mongo {

    CmdLine cmdLine;    
    Database *database = 0;
    string mongosCommand;
    string ourHostname;
    OID serverID;
    bool dbexitCalled = false;

    bool inShutdown(){
        return dbexitCalled;
    }
    
    string getDbContext() {
        return "?";
    }

    bool haveLocalShardingInfo( const string& ns ){
        assert( 0 );
        return false;
    }
    
    void usage( char * argv[] ){
        out() << argv[0] << " usage:\n\n";
        out() << " -v+  verbose 1: general 2: more 3: per request 4: more\n";
        out() << " --port <portno>\n";
        out() << " --configdb <configdbname>,[<configdbname>,<configdbname>]\n";
        out() << endl;
    }

    class ShardingConnectionHook : public DBConnectionHook {
    public:
        virtual void onCreate( DBClientBase * conn ){
            conn->simpleCommand( "admin" , 0 , "switchtoclienterrors" );
        }
        virtual void onHandedOut( DBClientBase * conn ){
            ClientInfo::get()->addShard( conn->getServerAddress() );
        }
    } shardingConnectionHook;
    
    class ShardedMessageHandler : public MessageHandler {
    public:
        virtual ~ShardedMessageHandler(){}
        virtual void process( Message& m , AbstractMessagingPort* p ){
            assert( p );
            Request r( m , p );

            LastError * le = lastError.startRequest( m , r.getClientId() );
            assert( le );
            
            if ( logLevel > 5 ){
                log(5) << "client id: " << hex << r.getClientId() << "\t" << r.getns() << "\t" << dec << r.op() << endl;
            }
            try {
                setClientId( r.getClientId() );
                r.process();
            }
            catch ( DBException& e ){
                le->raiseError( e.getCode() , e.what() );
                
                m.data->id = r.id();
                log() << "UserException: " << e.what() << endl;
                if ( r.expectResponse() ){
                    BSONObj err = BSON( "$err" << e.what() << "code" << e.getCode() );
                    replyToQuery( QueryResult::ResultFlag_ErrSet, p , m , err );
                }
            }
        }
    };

    void sighandler(int sig){
        dbexit(EXIT_CLEAN, (string("received signal ") + BSONObjBuilder::numStr(sig)).c_str());
    }
    
    void setupSignals(){
        signal(SIGTERM, sighandler);
        signal(SIGINT, sighandler);
    }

    void init(){
        serverID.init();
        setupSIGTRAPforGDB();
        setupCoreSignals();
        setupSignals();
    }

    void start() {
        balancer.go();

        log() << "waiting for connections on port " << cmdLine.port << endl;
        //DbGridListener l(port);
        //l.listen();
        ShardedMessageHandler handler;
        MessageServer * server = createServer( cmdLine.port , &handler );
        server->run();
    }

    DBClientBase *createDirectClient(){
        uassert( 10197 ,  "createDirectClient not implemented for sharding yet" , 0 );
        return 0;
    }

    void printShardingVersionInfo(){
        log() << mongosCommand << " " << mongodVersion() << " starting (--help for usage)" << endl;
        printGitVersion();
        printSysInfo();
    }

} // namespace mongo

using namespace mongo;

#include <boost/program_options.hpp>

namespace po = boost::program_options;

int main(int argc, char* argv[], char *envp[] ) {
    static StaticObserver staticObserver;
    mongosCommand = argv[0];

    po::options_description options("Sharding options");
    po::options_description hidden("Hidden options");
    po::positional_options_description positional;
    
    CmdLine::addGlobalOptions( options , hidden );
    
    options.add_options()
        ( "configdb" , po::value<string>() , "1 or 3 comma separated config servers" )
        ( "test" , "just run unit tests" )
        ( "upgrade" , "upgrade meta data version" )
        ( "chunkSize" , po::value<int>(), "maximum amount of data per chunk" )
        ;
    

    // parse options
    po::variables_map params;
    if ( ! CmdLine::store( argc , argv , options , hidden , positional , params ) )
        return 0;
    
    if ( params.count( "help" ) ){
        cout << options << endl;
        return 0;
    }

    if ( params.count( "version" ) ){
        printShardingVersionInfo();
        return 0;
    }

    if ( params.count( "chunkSize" ) ){
        Chunk::MaxChunkSize = params["chunkSize"].as<int>() * 1024 * 1024;
    }

    if ( params.count( "test" ) ){
        logLevel = 5;
        UnitTest::runTests();
        cout << "tests passed" << endl;
        return 0;
    }
    
    if ( ! params.count( "configdb" ) ){
        out() << "error: no args for --configdb" << endl;
        return 4;
    }

    vector<string> configdbs;
    {
        string s = params["configdb"].as<string>();
        while ( true ){
            size_t idx = s.find( ',' );
            if ( idx == string::npos ){
                configdbs.push_back( s );
                break;
            }
            configdbs.push_back( s.substr( 0 , idx ) );
            s = s.substr( idx + 1 );
        }
    }

    if ( configdbs.size() != 1 && configdbs.size() != 3 ){
        out() << "need either 1 or 3 configdbs" << endl;
        return 5;
    }
    
    pool.addHook( &shardingConnectionHook );

    if ( argc <= 1 ) {
        usage( argv );
        return 3;
    }

    bool ok = cmdLine.port != 0 && configdbs.size();

    if ( !ok ) {
        usage( argv );
        return 1;
    }
    
    printShardingVersionInfo();
    
    if ( ! configServer.init( configdbs ) ){
        cout << "couldn't connectd to config db" << endl;
        return 7;
    }
    
    if ( ! configServer.ok() ){
        cout << "configServer startup check failed" << endl;
        return 8;
    }
    
    int configError = configServer.checkConfigVersion( params.count( "upgrade" ) );
    if ( configError ){
        if ( configError > 0 ){
            cout << "upgrade success!" << endl;
        }
        else {
            cout << "config server error: " << configError << endl;
        }
        return configError;
    }
    configServer.reloadSettings();
    
    init();
    start();
    dbexit( EXIT_CLEAN );
    return 0;
}

#undef exit
void mongo::dbexit( ExitCode rc, const char *why) {
    dbexitCalled = true;
    log() << "dbexit: " << why << " rc:" << rc << endl;
    ::exit(rc);
}
