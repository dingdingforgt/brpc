// Copyright (c) 2014 baidu-rpc authors.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: Li Zhaogeng (lizhaogeng01@baidu.com)

#ifdef BRPC_RDMA
#include <infiniband/verbs.h>
#endif
#include <butil/fd_utility.h>
#include <butil/logging.h>                   // CHECK, LOG
#include <butil/object_pool.h>               // return_object
#include <butil/rand_util.h>                 // RandBytes
#include <butil/sys_byteorder.h>             // NetToHost/HostToNet
#include <gflags/gflags.h>
#include "brpc/errno.pb.h"
#include "brpc/event_dispatcher.h"
#include "brpc/input_messenger.h"
#include "brpc/socket.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/rdma/rdma_endpoint.h"

namespace brpc {

DECLARE_bool(usercode_in_pthread);

namespace rdma {

// NOTE:
// The actual limitation is based on WR. Since the size of a WR is not
// fixed, the buffer size is only a rough recommendation.
DEFINE_int32(rdma_sbuf_size, 1048576, "Send buffer size for RDMA");
DEFINE_int32(rdma_rbuf_size, 1048576, "Recv buffer size for RDMA");
DEFINE_bool(rdma_recv_zerocopy, true, "Enable zerocopy for receive side");

// DO NOT change this value unless you know the safe value!!!
// This is the number of reserved WRs in SQ/RQ for pure ACK.
static const size_t RESERVED_WR_NUM = 3;

struct RdmaConnectRequestData {
    void Serialize(char* data) const {
        uint64_t* tmp = (uint64_t*)data;
        *tmp = butil::HostToNet64(sid);
        memcpy(data + sizeof(sid), rand_str, sizeof(rand_str));
        uint64_t* rq = (uint64_t*)(&data[sizeof(sid) + sizeof(rand_str)]);
        *rq = butil::HostToNet32(rq_size);
        uint64_t* sq = (uint64_t*)(&data[Length() - sizeof(sq_size)]);
        *sq = butil::HostToNet32(sq_size);
    }

    void Deserialize(char* data) {
        sid = butil::NetToHost64(*(uint64_t*)data);
        memcpy(rand_str, data + sizeof(sid), RANDOM_LENGTH);
        rq_size = butil::NetToHost32(*(uint32_t*)
                  ((char*)data + sizeof(sid) + sizeof(rand_str)));
        sq_size = butil::NetToHost32(*(uint32_t*)((char*)data + sizeof(sid) +
                  sizeof(rand_str) + sizeof(rq_size)));
    }

    size_t Length() const {
        return sizeof(sid) + sizeof(rand_str) + sizeof(rq_size) + sizeof(sq_size);
    }

    uint64_t sid;
    char rand_str[RANDOM_LENGTH];
    uint32_t rq_size;
    uint32_t sq_size;
};

struct RdmaConnectResponseData {
    void Serialize(char* data) const {
        uint32_t* rq = (uint32_t*)data;
        *rq = butil::HostToNet32(rq_size);
        uint64_t* sq = (uint64_t*)(&data[sizeof(rq_size)]);
        *sq = butil::HostToNet32(sq_size);
    }

    void Deserialize(char* data) {
        rq_size = butil::NetToHost32(*(uint32_t*)data);
        sq_size = butil::NetToHost32(*(uint32_t*)((char*)data + sizeof(rq_size)));
    }

    size_t Length() const {
        return sizeof(rq_size) + sizeof(sq_size);
    }

    uint32_t rq_size;
    uint32_t sq_size;
};

RdmaEndpoint::RdmaEndpoint(Socket* s)
    : _socket(s)
    , _rcm(NULL)
    , _rcq(NULL)
    , _qp(NULL)
    , _status(UNINITIALIZED)
    , _sq_size(FLAGS_rdma_sbuf_size / butil::IOBuf::DEFAULT_PAYLOAD + 1)
    , _rq_size(FLAGS_rdma_rbuf_size / butil::IOBuf::DEFAULT_PAYLOAD + 1)
    , _sbuf()
    , _rbuf()
    , _handshake_buf()
    , _accumulated_ack(0)
    , _unsolicited(0)
    , _sq_current(0)
    , _sq_unsignaled(0)
    , _sq_sent(0)
    , _rq_received(0)
    , _local_window_capacity(_sq_size)
    , _remote_window_capacity(_rq_size)
    , _window_size(_sq_size)
    , _new_rq_wrs(0)
    , _remote_sid(0)
    , _completion_queue()
{
    _pipefd[0] = -1;
    _pipefd[1] = -1;
    if (_sq_size < 16) {
        _sq_size = 16;
        _local_window_capacity = 16;
        _window_size.store(16, butil::memory_order_relaxed);
    }
    if (_rq_size < 16) {
        _rq_size = 16;
        _remote_window_capacity = 16;
    }
}

RdmaEndpoint::~RdmaEndpoint() {
    Reset();
}

void RdmaEndpoint::Reset() {
    if (_pipefd[0] >= 0) {
        close(_pipefd[0]);
        _pipefd[0] = -1;
    }
    if (_pipefd[1] >= 0) {
        close(_pipefd[1]);
        _pipefd[1] = -1;
    }

    DeallocateResources();

    _status = UNINITIALIZED;
    _sbuf.clear();
    _rbuf.clear();
    _accumulated_ack = 0;
    _unsolicited = 0;
    _sq_current = 0;
    _sq_unsignaled = 0;
    _local_window_capacity = _sq_size;
    _remote_window_capacity = _rq_size;
    _window_size.store(_sq_size, butil::memory_order_relaxed);
    _new_rq_wrs = 0;
    _remote_sid = 0;
    _sq_sent = 0;
    _rq_received = 0;
}

// Handshake protocol description:
//
// RDMA connection is totally independent from TCP connection, which
// is different from SSL.
// We still let the Socket establish a TCP connection first and then
// start the RDMA connection.
// The main reason of this TCP connection is to allow us to use legency
// tools (such as netstat, telnet and so on) to detect the status of
// connections and services.
// However, this design introduces a problem that we have to bind the
// RDMA connection with the associated Socket.
// Currently, we solve this problem in the following way:
// 1. The client side writes a hello message to the TCP fd, including
//    a magic string (RDMA) and a random string.
// 2. After the server side reads the random string, it keeps the string
//    and replies with its corresponding SocketId.
// 3. After the client side receives the SocketId, it starts the RDMA
//    connection which carries the SocketId and the previous random string.
// 4. When accepting the RDMA connection, the server side checks the
//    SocketId and the random string. If this random number equals to the
//    one kept in the Socket specified by the SocketId, it will accept the
//    RDMA connection. Otherwise will ignore it.
// Of course the above protocol still has the risk to be attacked by a
// malicious client.
// However, since RDMA application often works in internal clusters (not
// open to external users), we do not think it is a severe problem
// currently.

ssize_t RdmaEndpoint::Handshake() {
    // First we try to read from TCP fd
    // Then we try to read from rdmcm fd
    // At last we try to read from pipe fd
    size_t max_len = std::max(HELLO_LENGTH, sizeof(SocketId));
    size_t read_len = 0;
    do { 
        ssize_t nr = _handshake_buf
            .append_from_file_descriptor(_socket->fd(), max_len);
        if ((nr < 0 && errno != EAGAIN) || nr == 0) {
            return nr;
        }
        if (nr > 0) {
            read_len += nr;
        }
        break;  // nothing to read
    } while (_handshake_buf.size() < max_len);

    RdmaCMEvent event = RDMACM_EVENT_NONE;
    if (read_len == 0) {
        if (_rcm) {
            event = _rcm->GetCMEvent();
        }
        if (event == RDMACM_EVENT_NONE) {
            if (_pipefd[0] < 0) {
                return -1;
            }
            char tmp = 0;  // we don't care about the content
            ssize_t nr = read(_pipefd[0], &tmp, 1);
            if (nr < 0) {
                return -1;
            }
            if (nr == 1) {
                event = RDMACM_EVENT_ACCEPT;
            }
        }
    }

    if (event == RDMACM_EVENT_OTHER || event == RDMACM_EVENT_ERROR) {
        errno = ERDMACM;
        return -1;
    }

    if (_socket->CreatedByConnect()) {
        return HandshakeAtClient(event);
    } else {
        return HandshakeAtServer(event);
    }
}

static int InitPipe(int pipefd[]) {
    CHECK(pipefd[0] < 0);
    CHECK(pipefd[1] < 0);

    int rc = pipe(pipefd);
    if (rc == 0) {
        butil::make_close_on_exec(pipefd[0]);
        butil::make_close_on_exec(pipefd[1]);
        if (butil::make_non_blocking(pipefd[0]) < 0) {
            rc = -1;
        }
        if (butil::make_non_blocking(pipefd[1]) < 0) {
            rc = -1;
        }
    }
    return rc;
}

int RdmaEndpoint::HandshakeAtServer(RdmaCMEvent event) {
    bool direct_pass = false;
    switch(_status) {
    case UNINITIALIZED: {
        if (event != RDMACM_EVENT_NONE) {
            errno = EPROTO;
            return -1;
        }

        char tmp[HELLO_LENGTH];
        _handshake_buf.copy_to(tmp, HELLO_LENGTH);
        if (strncmp(tmp, MAGIC_STR, MAGIC_LENGTH) != 0) {
            // Client may not use RDMA
            _socket->_read_buf.append(_handshake_buf);
            _handshake_buf.clear();
            _socket->_rdma_state = Socket::RDMA_OFF;
            return _socket->_read_buf.size();
        }
        memcpy(_rand_str, tmp + MAGIC_LENGTH, RANDOM_LENGTH);

        if (InitPipe(_pipefd) < 0) {
            return -1;
        }

        _handshake_buf.clear();
        _status = HELLO_S;
        uint64_t sid = butil::HostToNet64(_socket->id());
        char* tmp2 = (char*)&sid;
        int sid_len = sizeof(sid);
        ssize_t left_len = sid_len;
        // There is only a few bytes to write, and it is the beginning of a
        // connection. Thus in almost all cases we only need to write once.
        do {
            ssize_t nw = write(_socket->fd(),
                               tmp2 + sid_len - left_len, left_len);
            if (nw < 0 && errno != EAGAIN) {
                PLOG(WARNING) << "Fail to write on fd=" << _socket->fd();
                return -1;
            }
            left_len -= nw;
        } while (left_len > 0);

        break;
    }
    case HELLO_S: {
        if (event != RDMACM_EVENT_ACCEPT) {
            errno = EPROTO;
            return -1;
        }

        if (AllocateResources() < 0) {
            PLOG(WARNING) << "Fail to allocate resources for RDMA";
            return -1;
        }

        // Add rdmacm fd to event dispatcher
        if (GetGlobalEventDispatcher(_rcm->GetFD()).
                    AddConsumer(_socket->id(), _rcm->GetFD()) < 0) {
            PLOG(WARNING) << "Fail to add rdmacm fd into event dispatcher";
            return -1;
        }

        RdmaConnectResponseData res;
        res.rq_size = _rq_size;
        res.sq_size = _sq_size;
        char data[res.Length()];
        res.Serialize(data);

        _status = ACCEPTING;
        if (_rcm->Accept(data, res.Length()) < 0) {
            if (errno != EAGAIN) {
                return -1;
            }
            break;
        }
        direct_pass = true;
    }
    case ACCEPTING: {
        if (!direct_pass && event != RDMACM_EVENT_ESTABLISHED) {
            errno = EPROTO;
            return -1;
        }
        _status = ESTABLISHED;
        _socket->_rdma_state = Socket::RDMA_ON;
        break;
    }
    case ESTABLISHED: {
        if (event != RDMACM_EVENT_DISCONNECT) {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    default:
        errno = EPROTO;
        PLOG(ERROR) << "Incorrect RDMA handshake protocol";
        return -1;
    }

    errno = EINTR;  // retry to read from TCP fd and rdmacm fd
    return -1;
}

int RdmaEndpoint::StartHandshake() {
    CHECK(_status == UNINITIALIZED);

    // RDMA requires PFC and ECN configurations in network switches.
    // Thus mostly RDMA is only enabled in a cluster.
    // For a remote side out of the cluster, we should never use RDMA.
    // Here we check if the remote side is in the same cluster with the
    // local side according to the ip address. (We assume that the ip
    // address of the servers in one cluster share the same ip prefix.)
    if (!DestinationInRdmaCluster(
            butil::NetToHost64(butil::ip2int(_socket->remote_side().ip)))) {
        LOG(WARNING) << "Destination is not in current RDMA cluster";
        _socket->_rdma_state = Socket::RDMA_OFF;
        return 0;
    }
    _status = HELLO_C;

    char tmp[HELLO_LENGTH];
    memcpy(tmp, MAGIC_STR, MAGIC_LENGTH);
    butil::RandBytes(_rand_str, RANDOM_LENGTH);
    memcpy(tmp + MAGIC_LENGTH, _rand_str, RANDOM_LENGTH);
    ssize_t left_len = HELLO_LENGTH;

    // Make sure _status==HELLO_C
    _window_size.store(_sq_size, butil::memory_order_release);

    // There is only a few bytes to write, and it is the beginning of a
    // connection. Thus in almost all cases we only need to write once.
    do {
        ssize_t nw = write(_socket->fd(), 
                tmp + HELLO_LENGTH - left_len, left_len);
        if (nw < 0 && errno != EAGAIN) {
            PLOG(WARNING) << "Fail to write on fd=" << _socket->fd();
            return -1;
        }
        left_len -= nw;
    } while (left_len > 0);

    return 0;
}

int RdmaEndpoint::HandshakeAtClient(RdmaCMEvent event) {
    bool direct_pass = false;

    if (_status == UNINITIALIZED) {
        // Make sure _status!=UNINITIALIZED
        while (_window_size.load(butil::memory_order_acquire) == 0);
    }

    switch (_status) {
    case HELLO_C: {
        if (event != RDMACM_EVENT_NONE) {
            errno = EPROTO;
            return -1;
        }

        size_t sid_len = sizeof(SocketId);
        char tmp[sid_len];
        _handshake_buf.copy_to(tmp, sid_len);
        SocketId* sid_addr = (SocketId*)tmp;
        _remote_sid = butil::NetToHost64(*sid_addr);
        if (_remote_sid == 0) {
            // Server may not use RDMA
            _handshake_buf.clear();
            _socket->_rdma_state = Socket::RDMA_OFF;
            _socket->WakeAsEpollOut();
            break;
        }

        _handshake_buf.clear();

        _rcm = RdmaCommunicationManager::Create();
        if (!_rcm) {
            return -1;
        }

        // Add rdmacm fd to event dispatcher
        if (GetGlobalEventDispatcher(_rcm->GetFD()).
                    AddConsumer(_socket->id(), _rcm->GetFD()) < 0) {
            PLOG(WARNING) << "Fail to add rdmacm fd into event dispatcher";
            return -1;
        }

        _status = ADDR_RESOLVING;
        if (_rcm->ResolveAddr(_socket->_remote_side) < 0) {
            if (errno != EAGAIN) {
                return -1;
            }
            break;
        }
        direct_pass = true;
    }
    case ADDR_RESOLVING: {
        if (!direct_pass && event != RDMACM_EVENT_ADDR_RESOLVED) {
            errno = EPROTO;
            return -1;
        }
        _status = ROUTE_RESOLVING;
        if (_rcm->ResolveRoute() < 0) {
            if (errno != EAGAIN) {
                return -1;
            }
            break;
        }
        direct_pass = true;
    }
    case ROUTE_RESOLVING: {
        if (!direct_pass && event != RDMACM_EVENT_ROUTE_RESOLVED) {
            errno = EPROTO;
            return -1;
        }
        if (AllocateResources() < 0) {
            PLOG(WARNING) << "Fail to allocate resources for RDMA";
            return -1;
        }

        RdmaConnectRequestData req;
        req.sid = _remote_sid;
        memcpy(req.rand_str, _rand_str, RANDOM_LENGTH);
        req.rq_size = _rq_size;
        req.sq_size = _sq_size;
        char data[req.Length()];
        req.Serialize(data);

        _status = CONNECTING;
        if (_rcm->Connect(data, req.Length()) < 0) {
            if (errno != EAGAIN) {
                return -1;
            }
            break;
        }
        direct_pass = true;
    }
    case CONNECTING: {
        if (!direct_pass && event != RDMACM_EVENT_ESTABLISHED) {
            errno = EPROTO;
            return -1;
        }
        void* data = _rcm->GetConnData();
        if (!data) {
            errno = EPROTO;
            return -1;
        }
        RdmaConnectResponseData res;
        res.Deserialize((char*)data);
        if (res.rq_size < _sq_size) {
            _local_window_capacity = res.rq_size;
            _window_size.store(res.rq_size, butil::memory_order_relaxed);
        }
        if (res.sq_size < _rq_size) {
            _remote_window_capacity = res.sq_size;
        }

        _status = ESTABLISHED;
        _socket->_rdma_state = Socket::RDMA_ON;
        _socket->WakeAsEpollOut();
        break;
    }
    case ESTABLISHED: {
        if (event != RDMACM_EVENT_DISCONNECT) {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    default:
        errno = EPROTO;
        PLOG(ERROR) << "Incorrect RDMA handshake protocol";
        return -1;
    }

    errno = EINTR;  // retry to read from TCP fd and rdmacm fd
    return -1;
}

bool RdmaEndpoint::IsWritable() const {
    return _window_size.load(butil::memory_order_relaxed) > 0;
}

// RdmaIOBuf inherits from IOBuf to provide a new function.
// The reason is that we need to use some protected member function of IOBuf.
class RdmaIOBuf : public butil::IOBuf {
friend class RdmaEndpoint;
private:
    // Cut the current IOBuf to ibv_sge list and `to' for at most first max_sge
    // blocks or first max_len bytes.
    // Return: the bytes included in the sglist, or -1 if failed
    ssize_t cut_into_sglist_and_iobuf(void* sglist, butil::IOBuf* to,
            size_t max_sge, size_t max_len, uint32_t* lkey) {
#ifndef BRPC_RDMA
        CHECK(false) << "This should not happen";
        return 0;
#else
        ibv_sge* list = (ibv_sge*)sglist;
        size_t len = 0;
        const size_t nref = _ref_num();
        size_t num = (nref < max_sge) ? nref : max_sge;
        for (size_t i = 0; i < num; ++i) {
            if (len == max_len) {
                break;
            }
            butil::IOBuf::BlockRef const& r = _ref_at(i);
            uint32_t this_lkey = GetLKey(backing_block(i).data());
            if (*lkey == 0) {
                *lkey = this_lkey;
            } else if (this_lkey != *lkey) {
                break;
            }
            char* start = (char*)backing_block(i).data();
            if (*lkey == 0) {
                // This block is not in the registered memory. It may be
                // allocated before we call GlobalRdmaInitializeOrDie. We try
                // to copy this block into the block_pool.
                CHECK(i == 0);  // should be the first block
                size_t append_len = (r.length < max_len) ? r.length : max_len;
                append_len = (append_len < butil::IOBuf::DEFAULT_PAYLOAD) ?
                    append_len : butil::IOBuf::DEFAULT_PAYLOAD;
                RdmaIOBuf tmp;
                if (tmp.append(start, append_len) < 0) {
                    return -1;
                }
                len = tmp.cut_into_sglist_and_iobuf(&list[i], to,
                        max_sge, append_len, lkey);
                cutn(&tmp, len);
                return len;
            }
            if (len + r.length > max_len) {
                if (r.length <= butil::IOBuf::DEFAULT_PAYLOAD) {
                    // Leave it for the next WR to avoid spliting blocks
                    break;
                } else {
                    // Split the block to comply with size for receiving
                    list[i].length = max_len - len;
                    len = max_len;
                }
            } else {
                list[i].length = r.length;
                len += r.length;
            }
            list[i].addr = (uint64_t)start;
            list[i].lkey = *lkey;
        }
        if (len > 0) {
            cutn(to, len);
        }
        return len;
#endif
    }
};

// Note this function is coupled with the implementation of IOBuf
ssize_t RdmaEndpoint::DoCutFromIOBufList(
        butil::IOBuf** from, size_t ndata, butil::IOBuf* to, uint32_t imm) {
#ifndef BRPC_RDMA
    CHECK(false) << "This should not happen";
    return -1;
#else
    CHECK(from != NULL);
    CHECK(to != NULL);
    CHECK(ndata > 0);

    size_t total_len = 0;
    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    size_t current = 0;
    RdmaIOBuf* data = (RdmaIOBuf*)from[current];
    wr.wr_id = _socket->id();
    int max_sge = GetRdmaMaxSge();
    ibv_sge sglist[max_sge];
    wr.sg_list = sglist;
    wr.opcode = IBV_WR_SEND_WITH_IMM;
    wr.imm_data = butil::HostToNet32(imm);
    size_t sge_index = 0;
    uint32_t lkey = 0;
    while (sge_index < (uint32_t)max_sge &&
           total_len < butil::IOBuf::DEFAULT_PAYLOAD) {
        if (data->size() == 0) {
            // The current IOBuf is empty, find next one
            ++current;
            if (current == ndata) {
                break;
            }
            data = (RdmaIOBuf*)from[current];
            continue;
        }

        ssize_t len = data->cut_into_sglist_and_iobuf(
                &sglist[sge_index], to, max_sge - sge_index,
                butil::IOBuf::DEFAULT_PAYLOAD - total_len, &lkey);
        if (len < 0) {
            return -1;
        }
        if (len == 0) {
            // It happens when:
            // 1. lkey is not same with the next block
            // 2. The next block is a full block
            break;
        }
        total_len += len;
        sge_index = to->backing_block_num();
    }
    wr.num_sge = sge_index;

    if (total_len <= 64) {
        wr.send_flags |= IBV_SEND_INLINE;
    }

    // Avoid too much recv completion event to reduce the cpu overhead
    bool solicited = false;
    if (current > 0 || data->size() == 0) {
        // At least one message is finished
        solicited = true;
    } else {
        ++_unsolicited;
        _accumulated_ack += imm;
        if (_unsolicited > _local_window_capacity / 4) {
            // Make sure the recv side can be signaled to return ack
            solicited = true;
        } else if (_accumulated_ack > _remote_window_capacity / 4) {
            // Make sure the recv side can be signaled to handle ack
            solicited = true;
        }
    }
    if (solicited) {
        wr.send_flags |= IBV_SEND_SOLICITED;
        _unsolicited = 0;
        _accumulated_ack = 0;
    }

    // Avoid too much send completion event to reduce the CPU overhead
    ++_sq_unsignaled;
    if (_sq_unsignaled >= _local_window_capacity / 4) {
        // Refer to:
        // http::www.rdmamojo.com/2014/06/30/working-unsignaled-completions/
        wr.send_flags |= IBV_SEND_SIGNALED;
        _sq_unsignaled = 0;
    }

    ibv_send_wr* bad = NULL;
    if (ibv_post_send((ibv_qp*)_qp, &wr, &bad) < 0) {
        // We use other way to guarantee the Send Queue is not full.
        // So we just consider this error as an unrecoverable error.
        PLOG(WARNING) << "Fail to ibv_post_send";
        return -1;
    }

    return total_len;
#endif
}

ssize_t RdmaEndpoint::CutFromIOBufList(
        butil::IOBuf** data_list, size_t ndata) {
    if (_window_size.load(butil::memory_order_relaxed) == 0) {
        // Must wait until window is not empty
        errno = EAGAIN;
        return -1;
    }

    CHECK(_sbuf[_sq_current].size() == 0);

    ssize_t nw = DoCutFromIOBufList(data_list, ndata, &_sbuf[_sq_current],
            _new_rq_wrs.exchange(0, butil::memory_order_relaxed));
    ++_sq_current;
    if (_sq_current == _sq_size) {
        _sq_current = 0;
    }

    // Update counters
    _window_size.fetch_sub(1, butil::memory_order_relaxed);

    return nw;
}

int RdmaEndpoint::SendImm(uint32_t imm) {
#ifndef BRPC_RDMA
    CHECK(false) << "This should not happen";
    return -1;
#else
    if (imm == 0) {
        return 0;
    }

    ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = _socket->id();
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.imm_data = butil::HostToNet32(imm);
    wr.send_flags |= IBV_SEND_SOLICITED;
    wr.send_flags |= IBV_SEND_SIGNALED;

    ibv_send_wr* bad = NULL;
    if (ibv_post_send((ibv_qp*)_qp, &wr, &bad) < 0) {
        // We use other way to guarantee the Send Queue is not full.
        // So we just consider this error as an unrecoverable error.
        PLOG(WARNING) << "Fail to ibv_post_send";
        return -1;
    }
    return 0;
#endif
}

ssize_t RdmaEndpoint::HandleCompletion(RdmaCompletion& rc) {
    // NOTE:
    // This function may be called before the server handles the rdmacm event
    // RDMACM_EVENT_ESTABLISHED. So we force modifying this state here.
    _socket->_rdma_state = Socket::RDMA_ON;

    switch (rc.type) {
    case RDMA_EVENT_WRITE:  // send completion of pure ACK
    case RDMA_EVENT_SEND: {  // send completion of data
        // Do nothing
        break;
    }
    case RDMA_EVENT_RECV: {  // recv completion of data
        CHECK(rc.len > 0);
        // Please note that only the first rc.len bytes is valid
        if (FLAGS_rdma_recv_zerocopy) {
            butil::IOBuf tmp;
            _rbuf[_rq_received].cutn(&tmp, rc.len);
            _socket->_read_buf.append(tmp);
        } else {
            // Copy data when the receive data is really small
            _socket->_read_buf.append(_rbuf_data[_rq_received], rc.len);
        }
        // Do not break here
    }
    case RDMA_EVENT_RECV_WITH_IMM: {  // recv completion of pure ACK
        if (rc.imm > 0) {
            // Clear sbuf here because we ignore event wakeup for send completions
            uint32_t num = rc.imm;
            while (num > 0) {
                CHECK(_sbuf[_sq_sent].size() > 0);
                _sbuf[_sq_sent++].clear();
                if (_sq_sent == _sq_size) {
                    _sq_sent = 0;
                }
                --num;
            }
            // Update window
            if (_window_size.fetch_add(rc.imm, butil::memory_order_relaxed) == 0) {
                _socket->WakeAsEpollOut();
            }
        }
        // We must re-post recv WR
        if (PostRecv(1) < 0) {
            return -1;
        }
        if (rc.len > 0 && _new_rq_wrs.fetch_add(1, butil::memory_order_relaxed)
                                      > _remote_window_capacity / 2) {
            // Send a pure ACK
            SendImm(_new_rq_wrs.exchange(0, butil::memory_order_relaxed));
        }
        return rc.len;
    }
    case RDMA_EVENT_ERROR: {
        errno = ERDMA;
        return -1;
    }
    default:
        CHECK(false) << "This should not happen";
        return -1;
    }
    return 0;
}

int RdmaEndpoint::DoPostRecv(void* block, size_t block_size) {
#ifndef BRPC_RDMA
    CHECK(false) << "This should not happen";
    return -1;
#else
    ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    ibv_sge sge;
    sge.addr = (uint64_t)block;
    sge.length = block_size;
    sge.lkey = GetLKey((char*)block + butil::IOBuf::DEFAULT_PAYLOAD
                       - butil::IOBuf::DEFAULT_BLOCK_SIZE);
    wr.wr_id = _socket->id();
    wr.num_sge = 1;
    wr.sg_list = &sge;

    ibv_recv_wr* bad = NULL;
    if (ibv_post_recv((ibv_qp*)_qp, &wr, &bad) < 0) {
        PLOG(WARNING) << "Fail to ibv_post_recv";
        return -1;
    }
    return 0;
#endif
}

int RdmaEndpoint::PostRecv(uint32_t num) {
    if (num == 0) {
        return 0;
    }
    // We do the post repeatedly from the _rbuf[_rq_received].
    do {
        if (FLAGS_rdma_recv_zerocopy || _rbuf[_rq_received].empty()) {
            _rbuf[_rq_received].clear();
            butil::IOBufAsZeroCopyOutputStream os(
                    &_rbuf[_rq_received], butil::IOBuf::DEFAULT_BLOCK_SIZE);
            int size = 0;
            if (!os.Next(&_rbuf_data[_rq_received], &size) ||
                    (uint64_t)size < butil::IOBuf::DEFAULT_PAYLOAD) {
                // Memory is not enough for preparing a block
                errno = ENOMEM;
                return -1;
            }
        }
        if (DoPostRecv(_rbuf_data[_rq_received], butil::IOBuf::DEFAULT_PAYLOAD) < 0) {
            _rbuf[_rq_received].clear();
            return -1;
        }
        --num;
        ++_rq_received;
        if (_rq_received == _rq_size + RESERVED_WR_NUM) {
            _rq_received = 0;
        }
    } while (num > 0);
    return 0;
}

int RdmaEndpoint::AllocateResources() {
#ifndef BRPC_RDMA
    CHECK(false) << "This should not happen";
    return -1;
#else
    CHECK(_rcm != NULL);

    // The capacity size of CQ is not easy to estimate.
    // Empirically, we use twice the sum of SQ+RQ size.
    _rcq = RdmaCompletionQueue::GetOne(_socket, 2 * (_sq_size + _rq_size));
    if (!_rcq) {
        return -1;
    }
    if (_rcq->IsShared()) {
        bthread::ExecutionQueueOptions options;
        options.bthread_attr = FLAGS_usercode_in_pthread ?
                               BTHREAD_ATTR_PTHREAD : BTHREAD_ATTR_NORMAL;
        options.bthread_attr.keytable_pool = _socket->_keytable_pool;
        if (bthread::execution_queue_start(
                &_completion_queue, &options, CompletionThread, this) < 0) {
            return -1;
        }
    }

    _qp = _rcm->CreateQP(_sq_size + RESERVED_WR_NUM,
                         _rq_size + RESERVED_WR_NUM,
                         (ibv_cq*)_rcq->GetCQ(), _socket->id());

    if (!_qp) {
        return -1;
    }
    // Reserve blocks for _sbuf and _rbuf for flow control
    _sbuf.resize(_sq_size);
    _rbuf.resize(_rq_size + RESERVED_WR_NUM);
    _rbuf_data.resize(_rq_size + RESERVED_WR_NUM);

    return PostRecv(_rbuf.size());
#endif
}

void RdmaEndpoint::DeallocateResources() {
    if (bthread::execution_queue_address(_completion_queue) != NULL) {
        bthread::execution_queue_stop(_completion_queue);
        // Do not join the execution queue, which may incur deadlock.
        // In fact, the execution thread must have jumpped out the loop
        // if we get here.
    }
    _sbuf.clear();
    _rbuf.clear();
    _rbuf_data.clear();

    delete _rcm;
    _rcm = NULL;
    if (_rcq) {
        if (_rcq->IsShared()) {
            _rcq->Release();
        } else {
            delete _rcq;
        }
        _rcq = NULL;
    }
    _qp = NULL;
}

int RdmaEndpoint::InitializeFromAccept(
        RdmaCommunicationManager* rcm, char* data, size_t len) {
    if (!data || len == 0) {
        return -1;
    }

    // Find the associated Socket
    RdmaConnectRequestData req;
    req.Deserialize(data);
    SocketUniquePtr s;
    if (Socket::Address(req.sid, &s) < 0) {
        LOG_EVERY_SECOND(WARNING) << "Invalid Socket id for rdma_accept";
        return -1;
    }

    rdma::RdmaEndpoint* ep = s->_rdma_ep;
    if (!ep) {
        LOG_EVERY_SECOND(WARNING) << "Try to use a Socket not using RDMA";
        // Do not set the Socket to failed because it may be an attack
        return -1;
    }

    // Check validity of random number
    if (memcmp(ep->_rand_str, req.rand_str, RANDOM_LENGTH) != 0) {
        LOG_EVERY_SECOND(WARNING) << "Random number is not matched";
        // Do not set the Socket to failed because it may be an attack
        return -1;
    }

    if (ep->_rcm) {
        LOG_EVERY_SECOND(WARNING) << "RDMA connection already exist";
        // Do not set the Socket to failed because it may be an attack
        return -1;
    }
    ep->_rcm = rcm;

    if (GetGlobalEventDispatcher(ep->_pipefd[0])
            .AddConsumer(s->id(), ep->_pipefd[0]) < 0) {
        const int saved_errno = errno;
        s->SetFailed(saved_errno, "Fail to add pipe fd to event dispatcher");
        return -1;
    }
    
    if (ep->_sq_size > req.rq_size) {
        ep->_local_window_capacity = req.rq_size;
        ep->_window_size.store(req.rq_size, butil::memory_order_relaxed);
    }
    if (ep->_rq_size > req.sq_size) {
        ep->_remote_window_capacity = req.sq_size;
    }

    char tmp = 0;  // we don't care about the content
    ssize_t nw = -1;
    do {
        nw = write(ep->_pipefd[1], &tmp, 1);  // wake the Handshake
        if (nw < 0 && errno != EAGAIN) {
            return -1;
        }
    } while (nw < 1);

    return 0;
}

int RdmaEndpoint::CompletionThread(void* arg,
        bthread::TaskIterator<RdmaCompletion*>& iter) {
    SocketUniquePtr s;
    InputMessenger::InputMessageClosure last_msg;

    RdmaEndpoint* ep = (RdmaEndpoint*)arg;
    for ( ; iter; ++iter) {
        RdmaCompletion* rc = *iter;
        CHECK(rc->socket != NULL);
        s.reset(rc->socket);
        if (iter.is_queue_stopped() || s->Failed()) {
            butil::return_object<RdmaCompletion>(rc);
            continue;
        }

        ssize_t nr = ep->HandleCompletion(*rc);
        butil::return_object<RdmaCompletion>(rc);
        if (nr < 0) {
            PLOG(WARNING) << "Fail to handle RDMA completion";
            s->SetFailed(errno, "Fail to handle RDMA completion");
            continue;
        }
        if (nr == 0) {
            continue;
        }

        const int64_t received_us = butil::cpuwide_time_us();
        const int64_t base_realtime = butil::gettimeofday_us() - received_us;
        InputMessenger* messenger = static_cast<InputMessenger*>(s->user());
        if (messenger->ProcessNewMessage(s.get(), nr, false,
                    received_us, base_realtime, last_msg) < 0) {
            continue;
        }
    }

    return 0;
}

int RdmaEndpoint::CompleteHandshake() {
    CHECK(_rcm != NULL);

    RdmaCMEvent event = _rcm->GetCMEvent();
    switch (event) {
    case RDMACM_EVENT_DISCONNECT:
    case RDMACM_EVENT_ESTABLISHED: {
        if (_socket->CreatedByConnect()) {
            return HandshakeAtClient(event);
        } else {
            return HandshakeAtServer(event);
        }
    }
    case RDMACM_EVENT_NONE: {
        break;
    }
    default:
        errno = ERDMACM;
    }

    return -1;
}

}  // namespace rdma
}  // namespace brpc

