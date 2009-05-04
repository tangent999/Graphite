#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <mpi.h>
#include <fcntl.h>
#include <sched.h>

#include "log.h"
#include "config.h"

#include "socktransport.h"

SockTransport::SockTransport()
   : m_barrier_count(0)
   , m_recvd_barrier_count(0)
{
   getProcInfo();
   initSockets();
   initBufferLists();

   m_global_node = new SockNode(GLOBAL_TAG, this);
}

void SockTransport::getProcInfo()
{
   m_num_procs = (SInt32)Config::getSingleton()->getProcessCount();

   const char *proc_index_str = getenv("CARBON_PROCESS_INDEX");
   LOG_ASSERT_ERROR(proc_index_str != NULL || m_num_procs == 1,
                    "Process index undefined with multiple processes.");

   if (proc_index_str)
      m_proc_index = atoi(proc_index_str);
   else
      m_proc_index = 0;

   LOG_ASSERT_ERROR(0 <= m_proc_index && m_proc_index < m_num_procs,
                    "Invalid process index: %d with num_procs: %d", m_proc_index, m_num_procs);

   Config::getSingleton()->setProcessNum(m_proc_index);
   LOG_PRINT("Process number set to %i", Config::getSingleton()->getCurrentProcessNum());
}

void SockTransport::initSockets()
{
   SInt32 my_port;

   LOG_PRINT("initSockets()");

   // -- server side
   my_port = BASE_PORT + m_proc_index;
   m_server_socket.listen(my_port, m_num_procs);

   // -- client side
   m_send_sockets = new Socket[m_num_procs];
   for (SInt32 proc = 0; proc < m_num_procs; proc++)
   {
      m_send_sockets[proc].connect("127.0.0.1", // FIXME: assume single machine
                                   BASE_PORT + proc);

      m_send_sockets[proc].send(&m_proc_index, sizeof(m_proc_index));
   }

   // -- accept connections
   m_recv_sockets = new Socket[m_num_procs];
   m_recv_locks = new Lock[m_num_procs];

   for (SInt32 proc = 0; proc < m_num_procs; proc++)
   {
      Socket sock = m_server_socket.accept();

      SInt32 proc_index;
      sock.recv(&proc_index, sizeof(proc_index), true);

      LOG_ASSERT_ERROR(0 <= proc_index && proc_index < m_num_procs,
                       "Connected process out of range: %d",
                       proc_index);

      m_recv_sockets[proc_index] = sock;
   }
}

void SockTransport::initBufferLists()
{
   m_num_lists
      = Config::getSingleton()->getTotalCores() // for cores
      + 1; // for global node

   m_buffer_lists = new buffer_list[m_num_lists];
   m_buffer_locks = new Lock[m_num_lists];
}

void SockTransport::updateBufferLists()
{
   for (SInt32 i = 0; i < m_num_procs; i++)
   {
      while (true)
      {
         UInt32 length;

         m_recv_locks[i].acquire();

         // first get packet length, abort if none available
         if (!m_recv_sockets[i].recv(&length, sizeof(length), false))
         {
            m_recv_locks[i].release();
            break;
         }

         // now receive packet
         SInt32 tag;
         m_recv_sockets[i].recv(&tag, sizeof(tag), true);

         Byte *buffer = new Byte[length];
         m_recv_sockets[i].recv(buffer, length, true);

         m_recv_locks[i].release();

         switch (tag)
         {
         case BARRIER_TAG:
            m_barrier_lock.acquire();
            m_recvd_barrier_count = *((SInt32*)buffer);
            LOG_PRINT("barrier recv %d", m_recvd_barrier_count);
            LOG_ASSERT_ERROR(m_recvd_barrier_count > m_barrier_count,
                             "Unexpected barrier counter value: %d <= %d", m_recvd_barrier_count, m_barrier_count);
            LOG_ASSERT_ERROR(i == (m_proc_index + m_num_procs - 1) % m_num_procs,
                             "Barrier update from unexpected process: %d", i);
            m_barrier_lock.release();
            delete [] buffer;
            break;

         case GLOBAL_TAG:
            tag = m_num_lists - 1;
            // fall through intentionally

         default:
            LOG_ASSERT_ERROR(tag >= 0, "Unexpected tag value: %d", tag);
            m_buffer_locks[tag].acquire();
            m_buffer_lists[tag].push_back(buffer);
            m_buffer_locks[tag].release();
            break;
         };
      }
   }
}

SockTransport::~SockTransport()
{
   LOG_PRINT("dtor");

   delete m_global_node;

   delete [] m_buffer_locks;
   delete [] m_buffer_lists;

   for (SInt32 i = 0; i < m_num_procs; i++)
   {
      m_recv_sockets[i].close();
      m_send_sockets[i].close();
   }
   m_server_socket.close();
   
   delete [] m_recv_locks;
   delete [] m_recv_sockets;
   delete [] m_send_sockets;
}

Transport::Node* SockTransport::createNode(core_id_t core_id)
{
   return new SockNode(core_id, this);
}

void SockTransport::barrier()
{
   // We implement a barrier using a ring of messages. We are using a
   // single socket for the entire process, however, and it is
   // multiplexed between many cores. So updates occur asynchronously
   // and possibly in other threads. That's what the counter business
   // is meant to take care of.
   //   There are two trips around the ring. The first trip blocks the
   // processes until everyone arrives. The second wakes them. This is
   // a low-performance implementation, but given how barriers are
   // used in the simulator, it should be OK.

   m_barrier_lock.acquire();
   LOG_PRINT("Entering barrier: %d %d", m_barrier_count, m_recvd_barrier_count);

   // receive ping from prev
   if (m_proc_index != 0)
   {
      while (m_barrier_count >= m_recvd_barrier_count)
      {
         m_barrier_lock.release();
         updateBufferLists();
         sched_yield();
         m_barrier_lock.acquire();
      }
   }

   ++m_barrier_count;

   // forward ping
   SInt32 message[] = { sizeof(SInt32), BARRIER_TAG, m_barrier_count };
   if (m_proc_index == m_num_procs - 1)
      ++message[2];
   m_send_sockets[(m_proc_index+1) % m_num_procs].send(message, sizeof(message));
   LOG_PRINT("barrier send %d", message[2]);

   // receive confirmation that all processes reached barrier from prev
   while (m_barrier_count >= m_recvd_barrier_count)
   {
      m_barrier_lock.release();
      updateBufferLists();
      sched_yield();
      m_barrier_lock.acquire();
   }

   ++m_barrier_count;

   // forward confirmation
   if (m_proc_index != m_num_procs - 1)
   {
      message[2] = m_barrier_count;
      m_send_sockets[(m_proc_index+1) % m_num_procs].send(message, sizeof(message));
      LOG_PRINT("barrier send %d", message[2]);
   }

   LOG_PRINT("Exiting barrier: %d %d", m_barrier_count, m_recvd_barrier_count);
   m_barrier_lock.release();
}

Transport::Node* SockTransport::getGlobalNode()
{
   return m_global_node;
}

// -- SockTransport::SockNode

SockTransport::SockNode::SockNode(core_id_t core_id, SockTransport *trans)
   : Node(core_id)
   , m_transport(trans)
{
}

SockTransport::SockNode::~SockNode()
{
}

void SockTransport::SockNode::globalSend(SInt32 dest_proc, 
                                         const void *buffer, 
                                         UInt32 length)
{
   send(dest_proc, GLOBAL_TAG, buffer, length);
}

void SockTransport::SockNode::send(core_id_t dest_core, 
                                   const void *buffer, 
                                   UInt32 length)
{
   int dest_proc = Config::getSingleton()->getProcessNumForCore(dest_core);
   send(dest_proc, dest_core, buffer, length);
}

Byte* SockTransport::SockNode::recv()
{
   LOG_PRINT("Entering recv");

   core_id_t tag = getCoreId();
   tag = (tag == GLOBAL_TAG) ? m_transport->m_num_lists - 1 : tag;

   buffer_list &list = m_transport->m_buffer_lists[tag];
   Lock &lock = m_transport->m_buffer_locks[tag];

   while (true)
   {
      m_transport->updateBufferLists();

      lock.acquire();

      if (!list.empty())
      {
         Byte *buffer = list.front();
         list.pop_front();
         lock.release();
         
         LOG_PRINT("Message recv'd");

         return buffer;
      }
      else
      {
         lock.release();
         sched_yield();
      }
   }
}

bool SockTransport::SockNode::query()
{
   m_transport->updateBufferLists();

   core_id_t tag = getCoreId();
   tag = (tag == GLOBAL_TAG) ? m_transport->m_num_lists - 1 : tag;

   buffer_list &list = m_transport->m_buffer_lists[tag];
   Lock &lock = m_transport->m_buffer_locks[tag];

   lock.acquire();
   bool result = !list.empty();
   lock.release();
   return result;
}

void SockTransport::SockNode::send(SInt32 dest_proc, 
                                   UInt32 tag, 
                                   const void *buffer, 
                                   UInt32 length)
{
   SInt32 pkt_len = length + sizeof(tag) + sizeof(length);

   Byte *pkt_buff = new Byte[pkt_len];

   Packet *p = (Packet*)pkt_buff;
   p->length = length;
   p->tag = tag;
   memcpy(&p->data, buffer, length);

   m_transport->m_send_sockets[dest_proc].send(pkt_buff, pkt_len);

   LOG_PRINT("Message sent.");

   delete [] pkt_buff;
}

// -- Socket

SockTransport::Socket::Socket()
   : m_socket(-1)
{
}

SockTransport::Socket::Socket(SInt32 sock)
   : m_socket(sock)
{
}

SockTransport::Socket::~Socket()
{
}

void SockTransport::Socket::listen(SInt32 port, SInt32 max_pending)
{
   SInt32 err;

   LOG_ASSERT_ERROR(m_socket == -1, "Listening on already-open socket: %d", m_socket);

   m_socket = ::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
   LOG_ASSERT_ERROR(m_socket >= 0, "Failed to create socket.");

   SInt32 on = 1;
   err = ::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
   LOG_ASSERT_ERROR(err >= 0, "Failed to set socket options.");

// Not needed
//    err = fcntl(m_server_socket, F_SETFL, O_NONBLOCK);
//    LOG_ASSERT_ERROR(err >= 0, "Failed to set non-blocking.");

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = INADDR_ANY;
   addr.sin_port = htons(port);

   err = ::bind(m_socket,
                (struct sockaddr *) &addr,
                sizeof(addr));
   LOG_ASSERT_ERROR(err >= 0, "Failed to bind");

   err = ::listen(m_socket,
                  max_pending);
   LOG_ASSERT_ERROR(err >= 0, "Failed to listen.");

   LOG_PRINT("Listening on socket: %d", m_socket);
}

SockTransport::Socket SockTransport::Socket::accept()
{
   struct sockaddr_in client;
   UInt32 client_len;
   SInt32 sock;

   memset(&client, 0, sizeof(client));
   client_len = sizeof(client);

   sock = ::accept(m_socket,
                   (struct sockaddr *)&client,
                   &client_len);
   LOG_ASSERT_ERROR(sock >= 0, "Failed to accept.");

   LOG_PRINT("Accepted connection %d on socket %d", sock, m_socket);

   return Socket(sock);
}

void SockTransport::Socket::connect(const char* addr, SInt32 port)
{
   SInt32 err;

   LOG_ASSERT_ERROR(m_socket == -1, "Connecting on already-open socket: %d", m_socket);
   
   // create socket
   m_socket = ::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
   LOG_ASSERT_ERROR(m_socket >= 0, "Failed to create socket -- addr: %s, port: %d.", addr, port);

   // lookup the hostname
   struct hostent *host;
   struct in_addr h_addr;
   if ((host = gethostbyname(addr)) == NULL)
   {
      LOG_ASSERT_ERROR("Lookup on host: %s failed!", addr);
   }
   h_addr.s_addr = *((unsigned long *) host->h_addr_list[0]);

   // connect
   struct sockaddr_in saddr;
   memset(&saddr, 0, sizeof(saddr));
   saddr.sin_family = AF_INET;
   saddr.sin_addr.s_addr = h_addr.s_addr;
   saddr.sin_port = htons(port);

   while (true)
   {
      err = ::connect(m_socket, (struct sockaddr *)&saddr, sizeof(saddr));

      //LOG_ASSERT_ERROR(err >= 0, "Failed to connect -- addr: %s, port: %d", addr, port);

      if (err >= 0)
         break;
      else
         sched_yield();
   }

   LOG_PRINT("Connected on socket %d to %s:%d", m_socket, addr, port);
}

void SockTransport::Socket::send(const void* buffer, UInt32 length)
{
   SInt32 sent;
   sent = ::send(m_socket, buffer, length, 0);
   LOG_ASSERT_ERROR(UInt32(sent) == length, "Failure sending packet on socket %d -- %d != %d", m_socket, sent, length);
}

bool SockTransport::Socket::recv(void *buffer, UInt32 length, bool block)
{
   SInt32 recvd;

   while (true)
   {
      recvd = ::recv(m_socket, buffer, length, MSG_DONTWAIT);

      if (recvd >= 1)
      {
         if (recvd < SInt32(length))
         {
            buffer = (void*)((Byte*)buffer + recvd);
            length -= recvd;

            // block to receive remainder of message
            recvd = ::recv(m_socket, buffer, length, 0);
         }

         LOG_ASSERT_ERROR(UInt32(recvd) == length,
                          "Didn't receive full message on socket %d -- %d != %d",
                          m_socket, recvd, length);
         return true;
      }
      else
      {
         if (!block)
         {
            return false;
         }
         else
         {
            sched_yield();
         }
      }
   }
}

void SockTransport::Socket::close()
{
   LOG_PRINT("Closing socket: %d", m_socket);

   SInt32 err;
   err = ::close(m_socket);
   LOG_ASSERT_WARNING(err >= 0, "Failed to close socket: %d", err);

   m_socket = -1;
}