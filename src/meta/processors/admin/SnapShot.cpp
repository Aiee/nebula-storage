/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "meta/processors/admin/SnapShot.h"
#include <cstdlib>
#include "meta/processors/Common.h"
#include "meta/ActiveHostsMan.h"
#include "meta/MetaServiceUtils.h"
#include "common/network/NetworkUtils.h"

namespace nebula {
namespace meta {
ErrorOr<cpp2::ErrorCode, std::unordered_map<GraphSpaceID, std::vector<cpp2::CheckpointInfo>>>
Snapshot::createSnapshot(const std::string& name) {
    auto retSpacesHosts = getSpacesHosts();
    if (!retSpacesHosts.ok()) {
        return cpp2::ErrorCode::E_STORE_FAILURE;
    }
    std::unordered_map<GraphSpaceID, std::vector<cpp2::CheckpointInfo>> info;
    auto spacesHosts = retSpacesHosts.value();
    for (const auto& spaceHosts : spacesHosts) {
        for (const auto& host : spaceHosts.second) {
            auto status = client_->createSnapshot(spaceHosts.first, name, host).get();
            if (!status.ok()) {
                return cpp2::ErrorCode::E_RPC_FAILURE;
            }
            info[spaceHosts.first].emplace_back(
                apache::thrift::FRAGILE, host, status.value());
        }
    }
    return info;
}

cpp2::ErrorCode Snapshot::dropSnapshot(const std::string& name,
                                       const std::vector<HostAddr>& hosts) {
    auto retSpacesHosts = getSpacesHosts();
    if (!retSpacesHosts.ok()) {
        return cpp2::ErrorCode::E_STORE_FAILURE;
    }
    auto spacesHosts = retSpacesHosts.value();
    for (const auto& spaceHosts : spacesHosts) {
        for (const auto& host : spaceHosts.second) {
            if (std::find(hosts.begin(), hosts.end(), host) != hosts.end()) {
                auto status = client_->dropSnapshot(spaceHosts.first, name, host).get();
                if (!status.ok()) {
                    auto msg = "failed drop checkpoint : \"%s\". on host %s. error %s";
                    auto error = folly::stringPrintf(msg,
                                                     name.c_str(),
                                                     host.toString().c_str(),
                                                     status.toString().c_str());
                    LOG(ERROR) << error;
                }
            }
        }
    }
    return cpp2::ErrorCode::SUCCEEDED;
}

cpp2::ErrorCode Snapshot::blockingWrites(storage::cpp2::EngineSignType sign) {
    auto retSpacesHosts = getSpacesHosts();
    if (!retSpacesHosts.ok()) {
        return cpp2::ErrorCode::E_STORE_FAILURE;
    }
    auto spacesHosts = retSpacesHosts.value();
    auto ret = cpp2::ErrorCode::SUCCEEDED;
    for (const auto& spaceHosts : spacesHosts) {
        for (const auto& host : spaceHosts.second) {
            LOG(INFO) << "will block write host: " << host;
            auto status = client_->blockingWrites(spaceHosts.first, sign, host).get();
            if (!status.ok()) {
                LOG(ERROR) << " Send blocking sign error on host : " << host;
                ret = cpp2::ErrorCode::E_BLOCK_WRITE_FAILURE;
                if (sign == storage::cpp2::EngineSignType::BLOCK_ON) {
                    break;
                }
            }
        }
    }
    return ret;
}

StatusOr<std::map<GraphSpaceID, std::set<HostAddr>>> Snapshot::getSpacesHosts() {
    folly::SharedMutex::ReadHolder rHolder(LockUtils::spaceLock());
    std::map<GraphSpaceID, std::set<HostAddr>> hostsByspaces;
    auto prefix = MetaServiceUtils::partPrefix();
    std::unique_ptr<kvstore::KVIterator> iter;
    auto kvRet = kv_->prefix(kDefaultSpaceId, kDefaultPartId, prefix, &iter);
    if (kvRet != kvstore::ResultCode::SUCCEEDED) {
        LOG(ERROR) << "Get hosts meta data error";
        return Status::Error("Get hosts meta data error");
    }
    for (; iter->valid(); iter->next()) {
        auto partHosts = MetaServiceUtils::parsePartVal(iter->val());
        auto space = MetaServiceUtils::parsePartKeySpaceId(iter->key());
        if (!spaces_.empty()) {
            auto it = spaces_.find(space);
            if (it == spaces_.end()) {
                continue;
            }
        }

        for (auto& ph : partHosts) {
            hostsByspaces[space].emplace(std::move(ph));
        }
    }
    return hostsByspaces;
}

}  // namespace meta
}  // namespace nebula

