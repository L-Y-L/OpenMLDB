//
// ns_client.h
// Copyright (C) 2017 4paradigm.com
// Author vagrant
// Date 2017-09-18
//


#ifndef RTIDB_NS_CLIENT_H
#define RTIDB_NS_CLIENT_H

#include <stdint.h>
#include <vector>
#include "rpc/rpc_client.h"
#include "proto/name_server.pb.h"

namespace rtidb {
namespace client {

struct TabletInfo {
    std::string endpoint;
    std::string state;
    uint64_t age;
};

class NsClient {

public:
    NsClient(const std::string& endpoint);

    int Init();

    bool ShowTablet(std::vector<TabletInfo>& tablets);
    
    bool ShowTable(const std::string& name, std::vector<::rtidb::nameserver::TableInfo>& tables);

    bool MakeSnapshot(const std::string& name, uint32_t pid);

    bool ShowOPStatus(::rtidb::nameserver::ShowOPStatusResponse& response);

    bool CreateTable(const ::rtidb::nameserver::TableInfo& table_info);

    bool DropTable(const std::string& name);

    bool AddReplica(const std::string& name, uint32_t pid, const std::string& endpoint);

    bool DelReplica(const std::string& name, uint32_t pid, const std::string& endpoint);
	
private:
    std::string endpoint_;
    ::rtidb::RpcClient<::rtidb::nameserver::NameServer_Stub> client_;
};

}
}

#endif /* !RTIDB_NS_CLIENT_H */
