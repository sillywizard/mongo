/* dbgrid/request.cpp

   Top level handling of requests (operations such as query, insert, ...)
*/

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
#include "server.h"

#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../db/stats/counters.h"

#include "../client/connpool.h"

#include "request.h"
#include "config.h"
#include "chunk.h"
#include "stats.h"

namespace mongo {

    Request::Request( Message& m, AbstractMessagingPort* p ) : 
        _m(m) , _d( m ) , _p(p){
        
        assert( _d.getns() );
        _id = _m.data->id;
        
        _clientId = p ? p->remotePort() << 16 : 0;
        _clientInfo = ClientInfo::get( _clientId );
        _clientInfo->newRequest( p );
        
        reset();
    }

    void Request::reset( bool reload ){
        _config = grid.getDBConfig( getns() );
        if ( reload )
            uassert( 10192 ,  "db config reload failed!" , _config->reload() );

        if ( _config->isSharded( getns() ) ){
            _chunkManager = _config->getChunkManager( getns() , reload );
            uassert( 10193 ,  (string)"no shard info for: " + getns() , _chunkManager );
        }
        else {
            _chunkManager = 0;
        }        

        _m.data->id = _id;
        
    }
    
    Shard Request::primaryShard() const {
        if ( _chunkManager ){
            if ( _chunkManager->numChunks() > 1 )
                throw UserException( 8060 , "can't call primaryShard on a sharded collection" );
            return _chunkManager->findChunk( _chunkManager->getShardKey().globalMin() ).getShard();
        }
        Shard s = _config->getShard( getns() );
        uassert( 10194 ,  "can't call primaryShard on a sharded collection!" , s.ok() );
        return s;
    }
    
    void Request::process( int attempt ){
        log(3) << "Request::process ns: " << getns() << " msg id:" << (int)(_m.data->id) << " attempt: " << attempt << endl;

        int op = _m.data->operation();
        assert( op > dbMsg );
        
        Strategy * s = SINGLE;
        _counter = &opsNonSharded;
        
        _d.markSet();
        
        if ( _chunkManager ){
            s = SHARDED;
            _counter = &opsSharded;
        }

        bool iscmd = false;
        if ( op == dbQuery ) {
            iscmd = isCommand();
            try {
                s->queryOp( *this );
            }
            catch ( StaleConfigException& staleConfig ){
                log() << staleConfig.what() << " attempt: " << attempt << endl;
                uassert( 10195 ,  "too many attempts to update config, failing" , attempt < 5 );
                
                sleepsecs( attempt );
                reset( true );
                _d.markReset();
                process( attempt + 1 );
                return;
            }
        }
        else if ( op == dbGetMore ) {
            s->getMore( *this );
        }
        else {
            s->writeOp( op, *this );
        }

        globalOpCounters.gotOp( op , iscmd );
        _counter->gotOp( op , iscmd );
    }
    
    bool Request::isCommand() const {
        int x = _d.getQueryNToReturn();
        return ( x == 1 || x == -1 ) && strstr( getns() , ".$cmd" );
    }

    void Request::gotInsert(){
        globalOpCounters.gotInsert();
        _counter->gotInsert();
    }
    
    ClientInfo::ClientInfo( int clientId ) : _id( clientId ){
        _cur = &_a;
        _prev = &_b;
        newRequest();
    }
    
    ClientInfo::~ClientInfo(){
        scoped_lock lk( _clientsLock );
        ClientCache::iterator i = _clients.find( _id );
        if ( i != _clients.end() ){
            _clients.erase( i );
        }
    }
    
    void ClientInfo::addShard( const string& shard ){
        _cur->insert( shard );
    }
    
    void ClientInfo::newRequest( AbstractMessagingPort* p ){

        if ( p ){
            string r = p->remote().toString();
            if ( _remote == "" )
                _remote = r;
            else if ( _remote != r ){
                stringstream ss;
                ss << "remotes don't match old [" << _remote << "] new [" << r << "]";
                throw UserException( 13134 , ss.str() );
            }
        }
        
        _lastAccess = (int) time(0);
        
        set<string> * temp = _cur;
        _cur = _prev;
        _prev = temp;
        _cur->clear();
    }
    
    void ClientInfo::disconnect(){
        _lastAccess = 0;
    }
        
    ClientInfo * ClientInfo::get( int clientId , bool create ){
        
        if ( ! clientId )
            clientId = getClientId();
        
        if ( ! clientId ){
            ClientInfo * info = _tlInfo.get();
            if ( ! info ){
                info = new ClientInfo( 0 );
                _tlInfo.reset( info );
            }
            info->newRequest();
            return info;
        }
        
        scoped_lock lk( _clientsLock );
        ClientCache::iterator i = _clients.find( clientId );
        if ( i != _clients.end() )
            return i->second;
        if ( ! create )
            return 0;
        ClientInfo * info = new ClientInfo( clientId );
        _clients[clientId] = info;
        return info;
    }
        
    map<int,ClientInfo*> ClientInfo::_clients;
    mongo::mutex ClientInfo::_clientsLock;
    boost::thread_specific_ptr<ClientInfo> ClientInfo::_tlInfo;

} // namespace mongo
