/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "nccl.h"
#include "core.h"
#include "utils.h"
#include "bootstrap.h"
#include "net.h"
#include <unistd.h>
#include <sys/types.h>
#include "proxy.h"

struct bootstrapRootArgs { // root参数
  struct ncclSocket* listenSock;
  uint64_t magic;
};

/* Init functions */
// bootstrap网卡信息
static char bootstrapNetIfName[MAX_IF_NAME_SIZE+1];
static union ncclSocketAddress bootstrapNetIfAddr;        // 静态全局的Socket地址
static int bootstrapNetInitDone = 0;
pthread_mutex_t bootstrapNetLock = PTHREAD_MUTEX_INITIALIZER;

ncclResult_t bootstrapNetInit() { // 跨机bootstrap网络初始化
  if (bootstrapNetInitDone == 0) {
    pthread_mutex_lock(&bootstrapNetLock);
    if (bootstrapNetInitDone == 0) {
      char* env = getenv("NCCL_COMM_ID");     // communitor ID, 一般初始化为 ip:port形式
      if (env) {
        union ncclSocketAddress remoteAddr;
        if (ncclSocketGetAddrFromString(&remoteAddr, env) != ncclSuccess) {
          WARN("Invalid NCCL_COMM_ID, please use format: <ipv4>:<port> or [<ipv6>]:<port> or <hostname>:<port>");
          return ncclInvalidArgument;
        }
        if (ncclFindInterfaceMatchSubnet(bootstrapNetIfName, &bootstrapNetIfAddr, &remoteAddr, MAX_IF_NAME_SIZE, 1) <= 0) { // 匹配该子网中只查找 1个
          WARN("NET/Socket : No usable listening interface found");
          return ncclSystemError;
        }
      } else {
        // 查找所有的interfaces
        int nIfs = ncclFindInterfaces(bootstrapNetIfName, &bootstrapNetIfAddr, MAX_IF_NAME_SIZE, 1); // 只查找1个
        if (nIfs <= 0) {
          WARN("Bootstrap : no socket interface found");
          return ncclInternalError;
        }
      }
      char line[SOCKET_NAME_MAXLEN+MAX_IF_NAME_SIZE+2];
      sprintf(line, " %s:", bootstrapNetIfName);
      ncclSocketToString(&bootstrapNetIfAddr, line+strlen(line));
      INFO(NCCL_INIT, "Bootstrap : Using%s", line);
      bootstrapNetInitDone = 1;
    }
    pthread_mutex_unlock(&bootstrapNetLock);
  }
  return ncclSuccess;
}

/* Socket Interface Selection type */
enum bootstrapInterface_t { findSubnetIf = -1, dontCareIf = -2 };

// Additional sync functions
static ncclResult_t bootstrapNetSend(struct ncclSocket* sock, void* data, int size) { // 发送
  NCCLCHECK(ncclSocketSend(sock, &size, sizeof(int)));
  NCCLCHECK(ncclSocketSend(sock, data, size));
  return ncclSuccess;
}
static ncclResult_t bootstrapNetRecv(struct ncclSocket* sock, void* data, int size) { // 接收info
  int recvSize;
  NCCLCHECK(ncclSocketRecv(sock, &recvSize, sizeof(int)));          // 接收一个int，首个4B说明后面的数据大小
  if (recvSize > size) {
    WARN("Message truncated : received %d bytes instead of %d", recvSize, size);
    return ncclInternalError;
  }
  NCCLCHECK(ncclSocketRecv(sock, data, std::min(recvSize, size)));  // 接收数据
  return ncclSuccess;
}

struct extInfo { // 用于交换信息
  int rank;
  int nranks;
  union ncclSocketAddress extAddressListenRoot;
  union ncclSocketAddress extAddressListen;
};

#include <sys/resource.h>

static ncclResult_t setFilesLimit() {
  struct rlimit filesLimit;
  SYSCHECK(getrlimit(RLIMIT_NOFILE, &filesLimit), "getrlimit");
  filesLimit.rlim_cur = filesLimit.rlim_max;
  SYSCHECK(setrlimit(RLIMIT_NOFILE, &filesLimit), "setrlimit");
  return ncclSuccess;
}

static void *bootstrapRoot(void* rargs) { // bootstrap root的线程: 等待一个TCP连接，recv magic, send magic
  struct bootstrapRootArgs* args = (struct bootstrapRootArgs*)rargs;
  struct ncclSocket* listenSock = args->listenSock;
  uint64_t magic = args->magic;
  ncclResult_t res = ncclSuccess;
  int nranks = 0, c = 0;
  struct extInfo info;
  union ncclSocketAddress *rankAddresses = NULL;
  union ncclSocketAddress *rankAddressesRoot = NULL; // for initial rank <-> root information exchange
  union ncclSocketAddress *zero = NULL;
  NCCLCHECKGOTO(ncclCalloc(&zero, 1), res, out);
  setFilesLimit();

  TRACE(NCCL_INIT, "BEGIN");
  /* Receive addresses from all ranks */
  do {
    struct ncclSocket sock;
    NCCLCHECKGOTO(ncclSocketInit(&sock), res, out); // 创建一个新的Socket， 同步
    NCCLCHECKGOTO(ncclSocketAccept(&sock, listenSock), res, out);             // 等待一个rank连接上来，并接收一个magic
    NCCLCHECKGOTO(bootstrapNetRecv(&sock, &info, sizeof(info)), res, out);    // 接收对端的 rank信息 {locl_rank, nrank, listenaddr, peeraddr}
    NCCLCHECKGOTO(ncclSocketClose(&sock), res, out);                          // 关闭当前rank上的连接

    if (c == 0) { // 首个rank发送过来的nrank
      nranks = info.nranks;
      NCCLCHECKGOTO(ncclCalloc(&rankAddresses, nranks), res, out);
      NCCLCHECKGOTO(ncclCalloc(&rankAddressesRoot, nranks), res, out);
    }

    if (nranks != info.nranks) {
      WARN("Bootstrap Root : mismatch in rank count from procs %d : %d", nranks, info.nranks);
      goto out;
    }

    if (memcmp(zero, &rankAddressesRoot[info.rank], sizeof(union ncclSocketAddress)) != 0) {
      WARN("Bootstrap Root : rank %d of %d ranks has already checked in", info.rank, nranks);
      goto out;
    }

    // Save the connection handle for that rank
    memcpy(rankAddressesRoot+info.rank, &info.extAddressListenRoot, sizeof(union ncclSocketAddress));   // peer rank的bootstrap网络
    memcpy(rankAddresses+info.rank, &info.extAddressListen, sizeof(union ncclSocketAddress));           // peer rank的数据网络

    ++c;
    TRACE(NCCL_INIT, "Received connect from rank %d total %d/%d",  info.rank, c, nranks);
  } while (c < nranks);
  TRACE(NCCL_INIT, "COLLECTED ALL %d HANDLES", nranks);

  // Send the connect handle for the next rank in the AllGather ring
  for (int r=0; r<nranks; ++r) { // 
    int next = (r+1) % nranks;   // 从rank0上，每个rank{i}， 通过bootstrap网络，发送rank{i + 1}的数据地址
    struct ncclSocket sock;
    NCCLCHECKGOTO(ncclSocketInit(&sock, rankAddressesRoot+r, magic, ncclSocketTypeBootstrap), res, out);
    NCCLCHECKGOTO(ncclSocketConnect(&sock), res, out);                                               
    NCCLCHECKGOTO(bootstrapNetSend(&sock, rankAddresses+next, sizeof(union ncclSocketAddress)), res, out);
    NCCLCHECKGOTO(ncclSocketClose(&sock), res, out);
  }
  TRACE(NCCL_INIT, "SENT OUT ALL %d HANDLES", nranks);

out:
  if (listenSock != NULL) {
    ncclSocketClose(listenSock);
    free(listenSock);
  }
  if (rankAddresses) free(rankAddresses);
  if (rankAddressesRoot) free(rankAddressesRoot);
  if (zero) free(zero);
  free(rargs);

  TRACE(NCCL_INIT, "DONE");
  return NULL;
}

ncclResult_t bootstrapCreateRoot(struct ncclBootstrapHandle* handle, bool idFromEnv) { // 创建nccl Socket
  struct ncclSocket* listenSock;
  struct bootstrapRootArgs* args;
  pthread_t thread;

  NCCLCHECK(ncclCalloc(&listenSock, 1)); // nccl socket分配
  NCCLCHECK(ncclSocketInit(listenSock, &handle->addr, handle->magic, ncclSocketTypeBootstrap, NULL, 0)); // 创建 bootstrap的TCP Socket
  NCCLCHECK(ncclSocketListen(listenSock));  // 启动监听
  NCCLCHECK(ncclSocketGetAddr(listenSock, &handle->addr));  // 获取监听地址

  NCCLCHECK(ncclCalloc(&args, 1));
  args->listenSock = listenSock;    // 设置监听地址
  args->magic = handle->magic;
  NEQCHECK(pthread_create(&thread, NULL, bootstrapRoot, (void*)args), 0); // 创建单独的线程，并detach该线程；进行bootstrap网络初始化，并recv等待交换magic信息
  ncclSetThreadName(thread, "NCCL BootstrapR");
  NEQCHECK(pthread_detach(thread), 0); // will not be pthread_join()'d
  return ncclSuccess;
}

ncclResult_t bootstrapGetUniqueId(struct ncclBootstrapHandle* handle) { // 
  memset(handle, 0, sizeof(ncclBootstrapHandle));
  NCCLCHECK(getRandomData(&handle->magic, sizeof(handle->magic)));      // 随机8字节数据填充

  char* env = getenv("NCCL_COMM_ID");       // 环境变量设置的NCCL_COMM_ID
  if (env) {
    INFO(NCCL_ENV, "NCCL_COMM_ID set by environment to %s", env);
    if (ncclSocketGetAddrFromString(&handle->addr, env) != ncclSuccess) { // 延迟bootstrap网络的创建
      WARN("Invalid NCCL_COMM_ID, please use format: <ipv4>:<port> or [<ipv6>]:<port> or <hostname>:<port>");
      return ncclInvalidArgument;
    }
  } else {
    memcpy(&handle->addr, &bootstrapNetIfAddr, sizeof(union ncclSocketAddress));  // 与bootstrap使用相同的地址
    NCCLCHECK(bootstrapCreateRoot(handle, false));
  }

  return ncclSuccess;
}

struct unexConn {
  int peer;
  int tag;
  struct ncclSocket sock;
  struct unexConn* next;
};

struct bootstrapState {
  struct ncclSocket listenSock;               // 当前监听网络
  struct ncclSocket ringRecvSocket;           // 被动连接到ring中上家的网络
  struct ncclSocket ringSendSocket;           // 主动连接到ring中的下家的网络
  union ncclSocketAddress* peerCommAddresses;
  union ncclSocketAddress* peerProxyAddresses;
  struct unexConn* unexpectedConnections;
  int cudaDev;
  int rank;
  int nranks;
  uint64_t magic;
  volatile uint32_t *abortFlag;
};

ncclResult_t bootstrapInit(struct ncclBootstrapHandle* handle, struct ncclComm* comm) {
  int rank = comm->rank;
  int nranks = comm->nRanks;
  struct bootstrapState* state;
  struct ncclSocket* proxySocket;
  ncclSocketAddress nextAddr;
  struct ncclSocket sock, listenSockRoot;
  struct extInfo info = { 0 };

  NCCLCHECK(ncclCalloc(&state, 1));
  state->rank = rank;
  state->nranks = nranks;
  state->abortFlag = comm->abortFlag;
  comm->bootstrap = state;
  comm->magic = state->magic = handle->magic;

  TRACE(NCCL_INIT, "rank %d nranks %d", rank, nranks);

  info.rank = rank;
  info.nranks = nranks;
  // Create socket for other ranks to contact me
  // 创建一个数据网络：用来接收来自rank 
  NCCLCHECK(ncclSocketInit(&state->listenSock, &bootstrapNetIfAddr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag));
  NCCLCHECK(ncclSocketListen(&state->listenSock));
  NCCLCHECK(ncclSocketGetAddr(&state->listenSock, &info.extAddressListen));

  // Create socket for root to contact me
  // 主线程创建一个bootstrap网络的监听地址：用来接收来自rank0 bootstrap网络发送过来的消息
  NCCLCHECK(ncclSocketInit(&listenSockRoot, &bootstrapNetIfAddr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag));
  NCCLCHECK(ncclSocketListen(&listenSockRoot));
  NCCLCHECK(ncclSocketGetAddr(&listenSockRoot, &info.extAddressListenRoot));

  // stagger connection times to avoid an overload of the root
  if (nranks > 128) { // 根据自己的rank，分别延时 rank ms，防止 rank0上的网络处理并发连接失败
    long msec = rank;
    struct timespec tv;
    tv.tv_sec = msec / 1000;
    tv.tv_nsec = 1000000 * (msec % 1000);
    TRACE(NCCL_INIT, "rank %d delaying connection to root by %ld msec", rank, msec);
    (void) nanosleep(&tv, NULL);
  }

  // send info on my listening socket to root
  NCCLCHECK(ncclSocketInit(&sock, &handle->addr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag));
  NCCLCHECK(ncclSocketConnect(&sock));                        // 连接到 rank0的bootstrap网络
  NCCLCHECK(bootstrapNetSend(&sock, &info, sizeof(info)));    // 发送自己的bootstrap网络和数据网络地址到rank0
  NCCLCHECK(ncclSocketClose(&sock));

  // get info on my "next" rank in the bootstrap ring from root
  // 从 rank0 接收 ring上 下一个 rank的 数据网络地址
  NCCLCHECK(ncclSocketInit(&sock));
  NCCLCHECK(ncclSocketAccept(&sock, &listenSockRoot));
  NCCLCHECK(bootstrapNetRecv(&sock, &nextAddr, sizeof(union ncclSocketAddress)));
  NCCLCHECK(ncclSocketClose(&sock));
  NCCLCHECK(ncclSocketClose(&listenSockRoot));
  // 连接到下一个rank的数据网络
  NCCLCHECK(ncclSocketInit(&state->ringSendSocket, &nextAddr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag));
  NCCLCHECK(ncclSocketConnect(&state->ringSendSocket));
  // Accept the connect request from the previous rank in the AllGather ring
  // 初始化数据网络，并等待ring的上一家进行连接
  NCCLCHECK(ncclSocketInit(&state->ringRecvSocket));
  NCCLCHECK(ncclSocketAccept(&state->ringRecvSocket, &state->listenSock));

  // AllGather all listen handlers
  // allgather 所有rank的地址
  NCCLCHECK(ncclCalloc(&state->peerCommAddresses, nranks));
  // 填充自己数据网络的地址
  NCCLCHECK(ncclSocketGetAddr(&state->listenSock, state->peerCommAddresses+rank));
  // allgather所有rank的数据面网络地址
  NCCLCHECK(bootstrapAllGather(state, state->peerCommAddresses, sizeof(union ncclSocketAddress)));

  // Create the service proxy
  NCCLCHECK(ncclCalloc(&state->peerProxyAddresses, nranks));

  // proxy is aborted through a message; don't set abortFlag
  NCCLCHECK(ncclCalloc(&proxySocket, 1));
  NCCLCHECK(ncclSocketInit(proxySocket, &bootstrapNetIfAddr, comm->magic, ncclSocketTypeProxy, comm->abortFlag));
  NCCLCHECK(ncclSocketListen(proxySocket));
  NCCLCHECK(ncclSocketGetAddr(proxySocket, state->peerProxyAddresses+rank));
  NCCLCHECK(bootstrapAllGather(state, state->peerProxyAddresses, sizeof(union ncclSocketAddress)));
  NCCLCHECK(ncclProxyInit(comm, proxySocket, state->peerProxyAddresses));

  TRACE(NCCL_INIT, "rank %d nranks %d - DONE", rank, nranks);

  return ncclSuccess;
}

ncclResult_t bootstrapSplit(struct ncclBootstrapHandle* handle, struct ncclComm* comm, struct ncclComm* parent, int color, int key, int* parentRanks)
{
  ncclResult_t ret = ncclSuccess;
  int rank = comm->rank;
  int nranks = comm->nRanks;
  int prev, next;
  ncclSocketAddress listenAddr, tmpAddr;
  struct ncclSocket* proxySocket;
  struct bootstrapState* state;

  NCCLCHECKGOTO(ncclCalloc(&state, 1), ret, fail);
  state->rank = rank;
  state->nranks = nranks;
  state->abortFlag = comm->abortFlag;
  comm->bootstrap = state;
  comm->magic = state->magic = handle->magic;

  prev = parentRanks[(rank-1+nranks)%nranks];
  next = parentRanks[(rank+1)%nranks];

  // Setup my sockets for the allgather ring and other p2p connections
  NCCLCHECKGOTO(ncclSocketInit(&state->listenSock, &bootstrapNetIfAddr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag, 0), ret, fail);
  NCCLCHECKGOTO(ncclSocketInit(&state->ringRecvSocket, NULL, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag, 0), ret, fail);

  // Create socket for other ranks to contact me
  NCCLCHECKGOTO(ncclSocketListen(&state->listenSock), ret, fail);

  // Get addr from next rank
  NCCLCHECKGOTO(ncclSocketGetAddr(&state->listenSock, &listenAddr), ret, fail);
  NCCLCHECKGOTO(bootstrapSend(parent->bootstrap, prev, -2, &listenAddr, sizeof(union ncclSocketAddress)), ret, fail);
  NCCLCHECKGOTO(bootstrapRecv(parent->bootstrap, next, -2, &tmpAddr, sizeof(union ncclSocketAddress)), ret, fail);

  NCCLCHECKGOTO(ncclSocketInit(&state->ringSendSocket, &tmpAddr, comm->magic, ncclSocketTypeBootstrap, comm->abortFlag, 0), ret, fail);
  NCCLCHECKGOTO(ncclSocketConnect(&state->ringSendSocket), ret, fail);
  // Accept the connect request from the previous rank in the AllGather ring
  NCCLCHECKGOTO(ncclSocketAccept(&state->ringRecvSocket, &state->listenSock), ret, fail);

  // AllGather all listen handlers
  NCCLCHECKGOTO(ncclCalloc(&state->peerCommAddresses, nranks), ret, fail);
  memcpy(state->peerCommAddresses+rank, &listenAddr, sizeof(union ncclSocketAddress));
  NCCLCHECKGOTO(bootstrapAllGather(state, state->peerCommAddresses, sizeof(union ncclSocketAddress)), ret, fail);

  if (parent->config.splitShare) {
    /* map local rank to top parent local rank. */
    for (int i = 0; i < nranks; ++i) {
      comm->topParentRanks[i] = parent->topParentRanks[parentRanks[i]];
    }
    comm->proxyState = parent->sharedRes->proxyState;
    ncclAtomicRefCountIncrement(&parent->sharedRes->proxyState->refCount);
  } else {
    // Create the service proxy
    NCCLCHECKGOTO(ncclCalloc(&state->peerProxyAddresses, nranks), ret, fail);
    NCCLCHECKGOTO(ncclCalloc(&proxySocket, 1), ret, fail);
    NCCLCHECKGOTO(ncclSocketInit(proxySocket, &bootstrapNetIfAddr, comm->magic, ncclSocketTypeProxy, comm->abortFlag, 0), ret, fail);
    NCCLCHECKGOTO(ncclSocketListen(proxySocket), ret, fail);
    NCCLCHECKGOTO(ncclSocketGetAddr(proxySocket, &tmpAddr), ret, fail);
    memcpy(state->peerProxyAddresses + rank, &tmpAddr, sizeof(union ncclSocketAddress));
    NCCLCHECKGOTO(bootstrapAllGather(state, state->peerProxyAddresses, sizeof(union ncclSocketAddress)), ret, fail);
    NCCLCHECKGOTO(ncclProxyInit(comm, proxySocket, state->peerProxyAddresses), ret, fail);
  }

  INFO(NCCL_INIT, "bootstrapSplit: rank %d nranks %d color %d key %d prev %d next %d - DONE", rank, nranks, color, key, prev, next);

exit:
  return ret;
fail:
  goto exit;
}

ncclResult_t bootstrapAllGather(void* commState, void* allData, int size) { // 
  struct bootstrapState* state = (struct bootstrapState*)commState;
  char* data = (char*)allData;
  int rank = state->rank;
  int nranks = state->nranks;

  TRACE(NCCL_INIT, "rank %d nranks %d size %d", rank, nranks, size);

  /* Simple ring based AllGather
   * At each step i receive data from (rank-i-1) from left
   * and send previous step's data from (rank-i) to right
   * ring的方式，先发送自己到下家，再接收上家。每次发送
   */
  for (int i=0; i<nranks-1; i++) {
    size_t rslice = (rank - i - 1 + nranks) % nranks;
    size_t sslice = (rank - i + nranks) % nranks;

    // Send slice to the right 先发送给ring的下家
    NCCLCHECK(bootstrapNetSend(&state->ringSendSocket, data+sslice*size, size));
    // Recv slice from the left 接收ring的上家
    NCCLCHECK(bootstrapNetRecv(&state->ringRecvSocket, data+rslice*size, size));
  }

  TRACE(NCCL_INIT, "rank %d nranks %d size %d - DONE", rank, nranks, size);
  return ncclSuccess;
}

ncclResult_t bootstrapSend(void* commState, int peer, int tag, void* data, int size) {
  ncclResult_t ret = ncclSuccess;
  struct bootstrapState* state = (struct bootstrapState*)commState;
  struct ncclSocket sock;

  NCCLCHECKGOTO(ncclSocketInit(&sock, state->peerCommAddresses+peer, state->magic, ncclSocketTypeBootstrap), ret, fail);
  NCCLCHECKGOTO(ncclSocketConnect(&sock), ret, fail);
  NCCLCHECKGOTO(bootstrapNetSend(&sock, &state->rank, sizeof(int)), ret, fail);
  NCCLCHECKGOTO(bootstrapNetSend(&sock, &tag, sizeof(int)), ret, fail);
  NCCLCHECKGOTO(bootstrapNetSend(&sock, data, size), ret, fail);

exit:
  NCCLCHECK(ncclSocketClose(&sock));
  return ret;
fail:
  goto exit;
}

ncclResult_t bootstrapBarrier(void* commState, int *ranks, int rank, int nranks, int tag) {
  if (nranks == 1) return ncclSuccess;
  TRACE(NCCL_INIT, "rank %d nranks %d tag %x - ENTER", rank, nranks, tag);

  /* Simple intra process barrier
   *
   * Based on the dissemination algorithm by Debra Hensgen, Raphael Finkel, and Udi Manbet,
   * "Two Algorithms for Barrier Synchronization," International Journal of Parallel Programming, 17(1):1-17, 1988"
   */
  int data[1];
  for (int mask=1; mask<nranks; mask<<=1) {
    int src = (rank - mask + nranks) % nranks;
    int dst = (rank + mask) % nranks;
    NCCLCHECK(bootstrapSend(commState, ranks[dst], tag, data, sizeof(data)));
    NCCLCHECK(bootstrapRecv(commState, ranks[src], tag, data, sizeof(data)));
  }

  TRACE(NCCL_INIT, "rank %d nranks %d tag %x - DONE", rank, nranks, tag);
  return ncclSuccess;
}

ncclResult_t bootstrapIntraNodeAllGather(void* commState, int *ranks, int rank, int nranks, void* allData, int size) {
  if (nranks == 1) return ncclSuccess;
  char* data = (char*)allData;
  TRACE(NCCL_INIT, "rank %d nranks %d size %d - ENTER", rank, nranks, size);

  for (int i=1; i<nranks; i++) {
    int src = (rank - i + nranks) % nranks;
    int dst = (rank + i) % nranks;
    NCCLCHECK(bootstrapSend(commState, ranks[dst], /*tag=*/i, data+rank*size, size));
    NCCLCHECK(bootstrapRecv(commState, ranks[src], /*tag=*/i, data+src*size, size));
  }

  TRACE(NCCL_INIT, "rank %d nranks %d size %d - DONE", rank, nranks, size);
  return ncclSuccess;
}

// IntraNode in-place Broadcast
ncclResult_t bootstrapIntraNodeBroadcast(void* commState, int *ranks, int rank, int nranks, int root, void* bcastData, int size) {
  if (nranks == 1) return ncclSuccess;
  TRACE(NCCL_INIT, "rank %d nranks %d root %d size %d - ENTER", rank, nranks, root, size);

  if (rank == root) {
    for (int i=0; i<nranks; i++) {
      if (i != root) NCCLCHECK(bootstrapSend(commState, ranks[i], /*tag=*/ranks[i], bcastData, size));
    }
  }
  else {
    NCCLCHECK(bootstrapRecv(commState, ranks[root], /*tag=*/ranks[rank], bcastData, size));
  }

  TRACE(NCCL_INIT, "rank %d nranks %d root %d size %d - DONE", rank, nranks, root, size);
  return ncclSuccess;
}

ncclResult_t unexpectedEnqueue(struct bootstrapState* state, int peer, int tag, struct ncclSocket* sock) {
  // New unex
  struct unexConn* unex;
  NCCLCHECK(ncclCalloc(&unex, 1));
  unex->peer = peer;
  unex->tag = tag;
  memcpy(&unex->sock, sock, sizeof(struct ncclSocket));

  // Enqueue
  struct unexConn* list = state->unexpectedConnections;
  if (list == NULL) {
    state->unexpectedConnections = unex;
    return ncclSuccess;
  }
  while (list->next) list = list->next;
  list->next = unex;
  return ncclSuccess;
}

ncclResult_t unexpectedDequeue(struct bootstrapState* state, int peer, int tag, struct ncclSocket* sock, int* found) {
  struct unexConn* elem = state->unexpectedConnections;
  struct unexConn* prev = NULL;
  *found = 0;
  while (elem) {
    if (elem->peer == peer && elem->tag == tag) {
      if (prev == NULL) {
        state->unexpectedConnections = elem->next;
      } else {
        prev->next = elem->next;
      }
      memcpy(sock, &elem->sock, sizeof(struct ncclSocket));
      free(elem);
      *found = 1;
      return ncclSuccess;
    }
    prev = elem;
    elem = elem->next;
  }
  return ncclSuccess;
}

static void unexpectedFree(struct bootstrapState* state) {
  struct unexConn* elem = state->unexpectedConnections;
  struct unexConn* prev = NULL;

  while (elem) {
    prev = elem;
    elem = elem->next;
    free(prev);
  }
  return;
}

// We can't know who we'll receive from, so we need to receive everything at once
ncclResult_t bootstrapRecv(void* commState, int peer, int tag, void* data, int size) {
  ncclResult_t ret = ncclSuccess;
  struct bootstrapState* state = (struct bootstrapState*)commState;
  struct ncclSocket sock;
  int newPeer, newTag;

  // Search unexpected connections first
  int found;
  NCCLCHECK(unexpectedDequeue(state, peer, tag, &sock, &found));
  if (found) {
    NCCLCHECKGOTO(bootstrapNetRecv(&sock, ((char*)data), size), ret, fail);
    goto exit;
  }

  // Then look for new connections
  while (1) {
    NCCLCHECKGOTO(ncclSocketInit(&sock), ret, fail);
    NCCLCHECKGOTO(ncclSocketAccept(&sock, &state->listenSock), ret, fail);
    NCCLCHECKGOTO(bootstrapNetRecv(&sock, &newPeer, sizeof(int)), ret, fail);
    NCCLCHECKGOTO(bootstrapNetRecv(&sock, &newTag, sizeof(int)), ret, fail);
    if (newPeer == peer && newTag == tag) {
      NCCLCHECKGOTO(bootstrapNetRecv(&sock, ((char*)data), size), ret, fail);
      goto exit;
    }
    // Unexpected connection. Save for later.
    NCCLCHECKGOTO(unexpectedEnqueue(state, newPeer, newTag, &sock), ret, fail);
  }
exit:
  NCCLCHECK(ncclSocketClose(&sock));
  return ret;
fail:
  goto exit;
}

ncclResult_t bootstrapClose(void* commState) {
  struct bootstrapState* state = (struct bootstrapState*)commState;
  if (state->unexpectedConnections != NULL) {
    unexpectedFree(state);
    if (*state->abortFlag == 0) {
      WARN("Unexpected connections are not empty");
      return ncclInternalError;
    }
  }

  NCCLCHECK(ncclSocketClose(&state->listenSock));
  NCCLCHECK(ncclSocketClose(&state->ringSendSocket));
  NCCLCHECK(ncclSocketClose(&state->ringRecvSocket));

  free(state->peerCommAddresses);
  free(state);

  return ncclSuccess;
}

ncclResult_t bootstrapAbort(void* commState) {
  struct bootstrapState* state = (struct bootstrapState*)commState;
  if (commState == NULL) return ncclSuccess;
  NCCLCHECK(ncclSocketClose(&state->listenSock));
  NCCLCHECK(ncclSocketClose(&state->ringSendSocket));
  NCCLCHECK(ncclSocketClose(&state->ringRecvSocket));
  free(state->peerCommAddresses);
  free(state->peerProxyAddresses);
  free(state);
  return ncclSuccess;
}
