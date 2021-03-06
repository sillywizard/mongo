// balance.h

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

#pragma once

#include "../pch.h"
#include "../util/background.h"
#include "../client/dbclient.h"

namespace mongo {
    
    class Balancer : public BackgroundJob {
    public:
        Balancer();
        
        void run();

    private:
        bool shouldIBalance( DBClientBase& conn );
        
        /**
         * @return true if everything is ok
         */
        bool checkOIDs();

        /**
         * @return number of collections balanced
         */
        int balance( DBClientBase& conn );
        bool balance( DBClientBase& conn , const string& ns , const BSONObj& data );
        
        void ping();
        void ping( DBClientBase& conn );

        BSONObj pickChunk( vector<BSONObj>& from, vector<BSONObj>& to );

        string _myid;
        time_t _started;
        int _balancedLastTime;
    };
    
    extern Balancer balancer;
}
