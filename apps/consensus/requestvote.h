/**
 * @file requestvote.h
 * @brief Handlers for requestvote RPC
 */

#include "consensus.h"

#ifndef REQUESTVOTE_H
#define REQUESTVOTE_H

// The requestvote request send via eRPC
struct erpc_requestvote_t {
  int node_id;
  msg_requestvote_t rv;
};

void requestvote_handler(ERpc::ReqHandle *req_handle, void *_context) {
  assert(req_handle != nullptr && _context != nullptr);
  auto *c = static_cast<AppContext *>(_context);
  assert(c->check_magic());

  const ERpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  assert(req_msgbuf->get_data_size() == sizeof(erpc_requestvote_t));

  auto *req = reinterpret_cast<erpc_requestvote_t *>(req_msgbuf->buf);
  assert(node_id_to_name_map.count(req->node_id) != 0);

  if (kAppVerbose) {
    printf("consensus: Received requestvote request from %s [%s].\n",
           node_id_to_name_map[req->node_id].c_str(),
           ERpc::get_formatted_time().c_str());
  }

  // This does a linear search, which is OK for a small number of Raft servers
  raft_node_t *requester_node = raft_get_node(c->server.raft, req->node_id);
  assert(requester_node != nullptr);

  c->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf,
                            sizeof(msg_requestvote_response_t));
  req_handle->prealloc_used = true;

  // req->rv is valid only for the duration of this handler, which is OK because
  // msg_requestvote_t does not contain any dynamically allocated members.
  int e = raft_recv_requestvote(c->server.raft, requester_node, &req->rv,
                                reinterpret_cast<msg_requestvote_response_t *>(
                                    req_handle->pre_resp_msgbuf.buf));
  assert(e == 0);
  _unused(e);

  c->rpc->enqueue_response(req_handle);
}

void requestvote_cont(ERpc::RespHandle *, void *, size_t);  // Fwd decl

// Raft callback for sending requestvote request
static int __raft_send_requestvote(raft_server_t *, void *, raft_node_t *node,
                                   msg_requestvote_t *m) {
  assert(node != nullptr);

  auto *conn = static_cast<peer_connection_t *>(raft_node_get_udata(node));
  assert(conn != nullptr && conn->session_num >= 0 && conn->c != nullptr);

  AppContext *c = conn->c;
  assert(c->check_magic());

  if (!c->rpc->is_connected(conn->session_num)) {
    printf("consensus: Cannot send requestvote request (disconnected).\n");
    return 0;
  }

  if (kAppVerbose) {
    printf("consensus: Sending requestvote request to node %s [%s].\n",
           node_id_to_name_map[raft_node_get_id(node)].c_str(),
           ERpc::get_formatted_time().c_str());
  }

  auto *req_info = new req_info_t();  // XXX: Optimize with pool
  req_info->req_msgbuf = c->rpc->alloc_msg_buffer(sizeof(erpc_requestvote_t));
  ERpc::rt_assert(req_info->req_msgbuf.buf != nullptr,
                  "Failed to allocate request MsgBuffer");

  req_info->resp_msgbuf =
      c->rpc->alloc_msg_buffer(sizeof(msg_requestvote_response_t));
  ERpc::rt_assert(req_info->resp_msgbuf.buf != nullptr,
                  "Failed to allocate response MsgBuffer");

  req_info->node = node;

  auto *erpc_requestvote =
      reinterpret_cast<erpc_requestvote_t *>(req_info->req_msgbuf.buf);
  erpc_requestvote->node_id = c->server.node_id;
  erpc_requestvote->rv = *m;

  size_t req_tag = reinterpret_cast<size_t>(req_info);
  int ret = c->rpc->enqueue_request(
      conn->session_num, static_cast<uint8_t>(ReqType::kRequestVote),
      &req_info->req_msgbuf, &req_info->resp_msgbuf, requestvote_cont, req_tag);

  assert(ret == 0 || ret == -EBUSY);  // We checked is_connected above
  if (ret == -EBUSY) c->server.stat_requestvote_enq_fail++;

  // If we failed to send a request, pretend as if we sent it. Raft will retry
  // when it times out. A large timeout is OK, since we don't care much about
  // perf for requestvote Rpcs.
  return 0;
}

void requestvote_cont(ERpc::RespHandle *resp_handle, void *_context,
                      size_t tag) {
  assert(resp_handle != nullptr && _context != nullptr);
  auto *c = static_cast<AppContext *>(_context);
  assert(c->check_magic());

  auto *req_info = reinterpret_cast<req_info_t *>(tag);
  assert(req_info->resp_msgbuf.get_data_size() ==
         sizeof(msg_requestvote_response_t));

  if (kAppVerbose) {
    printf("consensus: Received requestvote response from node %s [%s].\n",
           node_id_to_name_map[raft_node_get_id(req_info->node)].c_str(),
           ERpc::get_formatted_time().c_str());
  }

  auto *msg_requestvote_resp =
      reinterpret_cast<msg_requestvote_response_t *>(req_info->resp_msgbuf.buf);

  int e = raft_recv_requestvote_response(c->server.raft, req_info->node,
                                         msg_requestvote_resp);
  assert(e == 0);  // XXX: Doc says: Shutdown if e != 0
  _unused(e);

  c->rpc->free_msg_buffer(req_info->req_msgbuf);
  c->rpc->free_msg_buffer(req_info->resp_msgbuf);
  delete req_info;  // Free allocated memory, XXX: Remove when we use pool

  c->rpc->release_response(resp_handle);
}

#endif