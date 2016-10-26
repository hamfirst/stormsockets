
#include "StormSocketBackend.h"

#include <fstream>

#ifdef USE_MBED
#include "mbedtls\error.h"
#include "mbedtls\debug.h"
#endif

#ifdef _WINDOWS
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#endif

#include <stdexcept>

namespace StormSockets
{
  StormSocketBackend::StormSocketBackend(const StormSocketInitSettings & settings) :
    m_Allocator(settings.HeapSize, settings.BlockSize, true),
    m_MessageReaders(settings.MaxPendingOutgoingPacketsPerConnection * sizeof(StormMessageReaderData) * settings.MaxConnections, sizeof(StormMessageReaderData), false),
    m_MessageSenders(settings.MaxPendingOutgoingPacketsPerConnection * sizeof(StormMessageWriterData) * settings.MaxConnections, sizeof(StormMessageWriterData), false),
    m_ClosingConnectionQueue(settings.MaxConnections),
    m_Resolver(m_IOService)
  {
    m_NextAcceptorId = 0;
    m_MaxConnections = settings.MaxConnections;
    m_ThreadStopRequested = false;
    m_NumSendThreads = settings.NumSendThreads;
    m_NumIOThreads = settings.NumIOThreads;

    m_Connections = std::make_unique<StormSocketConnectionBase[]>(settings.MaxConnections);

    for (int index = 0; index < settings.MaxConnections; index++)
    {
      m_Connections[index].m_SlotGen = 0;
    }

    m_SendThreadSemaphores = std::make_unique<StormSemaphore[]>(settings.NumSendThreads);
    m_SendQueue = std::make_unique<StormMessageMegaQueue<StormSocketIOOperation>[]>(settings.NumSendThreads);
    m_SendQueueArray = std::make_unique<StormMessageMegaContainer<StormSocketIOOperation>[]>(settings.NumSendThreads * settings.MaxSendQueueElements);
    m_SendQueueIncdices = std::make_unique<StormGenIndex[]>(settings.NumSendThreads * settings.MaxSendQueueElements);

    m_OutputQueue = std::make_unique<StormMessageMegaQueue<StormMessageWriter>[]>(settings.MaxConnections);
    m_OutputQueueArray = std::make_unique<StormMessageMegaContainer<StormMessageWriter>[]>(settings.MaxConnections * settings.MaxPendingOutgoingPacketsPerConnection);
    m_OutputQueueIncdices = std::make_unique<StormGenIndex[]>(settings.MaxConnections * settings.MaxPendingOutgoingPacketsPerConnection);

    m_FreeQueue = std::make_unique<StormMessageMegaQueue<StormSocketFreeQueueElement>[]>(settings.MaxConnections);
    m_FreeQueueArray = std::make_unique<StormMessageMegaContainer<StormSocketFreeQueueElement>[]>(settings.MaxConnections * settings.MaxPendingFreeingPacketsPerConnection * 2);
    m_FreeQueueIncdices = std::make_unique<StormGenIndex[]>(settings.MaxConnections * settings.MaxPendingFreeingPacketsPerConnection * 2);
    m_MaxPendingFrees = settings.MaxPendingFreeingPacketsPerConnection * 2;

    m_CloseConnectionSemaphore.Init(settings.MaxConnections);
    m_CloseConnectionThread = std::thread(&StormSocketBackend::CloseSocketThread, this);

    m_FixedBlockSize = settings.BlockSize;

    for (int index = 0; index < settings.MaxConnections; index++)
    {
      m_OutputQueue[index].Init(m_OutputQueueIncdices.get(), m_OutputQueueArray.get(),
        index * settings.MaxPendingOutgoingPacketsPerConnection, settings.MaxPendingOutgoingPacketsPerConnection);

      m_FreeQueue[index].Init(m_FreeQueueIncdices.get(), m_FreeQueueArray.get(),
        index * settings.MaxPendingFreeingPacketsPerConnection * 2, settings.MaxPendingFreeingPacketsPerConnection * 2);
    }

    for (int index = 0; index < settings.NumSendThreads; index++)
    {
      m_SendQueue[index].Init(m_SendQueueIncdices.get(), m_SendQueueArray.get(),
        index * settings.MaxSendQueueElements, settings.MaxSendQueueElements);

      int semaphore_max = settings.MaxSendQueueElements + (settings.MaxConnections * settings.MaxPendingFreeingPacketsPerConnection) / settings.NumSendThreads;
      m_SendThreadSemaphores[index].Init(semaphore_max * 2);
    }

    m_ClientSockets = std::make_unique<std::experimental::optional<asio::ip::tcp::socket>[]>(settings.MaxConnections);

    // Start the io threads
    m_IOThreads = std::make_unique<std::thread[]>(m_NumIOThreads);
    for (int index = 0; index < m_NumIOThreads; index++)
    {
      m_IOThreads[index] = std::thread(&StormSocketBackend::IOThreadMain, this);
    }

    m_SendThreads = std::make_unique<std::thread[]>(m_NumSendThreads);
    for (int index = 0; index < m_NumSendThreads; index++)
    {
      m_SendThreads[index] = std::thread(&StormSocketBackend::SendThreadMain, this, index);
    }
  }

  StormSocketBackend::~StormSocketBackend()
  {
    std::unique_lock<std::mutex> acceptor_lock(m_AcceptorLock);
    m_Acceptors.clear();
    acceptor_lock.unlock();

    m_ThreadStopRequested = true;

    for (int index = 0; index < m_NumIOThreads; index++)
    {
      m_IOThreads[index].join();
    }

    for (int index = 0; index < m_NumSendThreads; index++)
    {
      m_SendThreadSemaphores[index].Release();
      m_SendThreads[index].join();
    }

    m_CloseConnectionSemaphore.Release();
    m_CloseConnectionThread.join();

    for (int index = 0; index < m_MaxConnections; index++)
    {
      auto & connection = GetConnection(index);
      if (connection.m_Used.test_and_set())
      {
        auto connection_id = StormSocketConnectionId(index, connection.m_SlotGen);
        FreeConnectionResources(connection_id);
        connection.m_Frontend->DisassociateConnectionId(connection_id);
      }
    }
  }

  StormSocketBackendAcceptorId StormSocketBackend::InitAcceptor(StormSocketFrontend * frontend, const StormSocketListenData & init_data)
  {
    std::unique_lock<std::mutex> guard(m_AcceptorLock);

    AcceptorData new_acceptor = { frontend,
      asio::ip::tcp::acceptor(m_IOService),
      asio::ip::tcp::socket(m_IOService)
    };

    int acceptor_id = m_NextAcceptorId;
    m_NextAcceptorId++;
    auto acceptor_pair = m_Acceptors.emplace(std::make_pair(acceptor_id, std::move(new_acceptor)));
    auto & acceptor = acceptor_pair.first->second;

    asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::from_string(init_data.LocalInterface), init_data.Port);
    acceptor.m_Acceptor.open(asio::ip::tcp::v4());
    acceptor.m_Acceptor.bind(endpoint);
    acceptor.m_Acceptor.listen();

    guard.unlock();

    PrepareToAccept(acceptor_id);
    return acceptor_id;
  }

  void StormSocketBackend::DestroyAcceptor(StormSocketBackendAcceptorId id)
  {
    std::lock_guard<std::mutex> guard(m_AcceptorLock);
    auto acceptor_itr = m_Acceptors.find(id);

    if (acceptor_itr != m_Acceptors.end())
    {
      m_Acceptors.erase(acceptor_itr);
    }
  }

  StormSocketConnectionId StormSocketBackend::RequestConnect(StormSocketFrontend * frontend, const char * ip_addr, int port, const void * init_data)
  {
    asio::ip::tcp::socket socket(m_IOService);

    auto connection_id = AllocateConnection(frontend, 0, port, true, init_data);

    if (connection_id == StormSocketConnectionId::InvalidConnectionId)
    {
      socket.close();
      return StormSocketConnectionId::InvalidConnectionId;
    }

    m_ClientSockets[connection_id].emplace(std::move(socket));

    asio::error_code ec;
    auto numerical_addr = asio::ip::address_v4::from_string(ip_addr, ec);

    if (!ec)
    {
      PrepareToConnect(connection_id, asio::ip::tcp::endpoint(numerical_addr, port));
    }
    else
    {
      asio::ip::tcp::resolver::query resolver_query(ip_addr, std::to_string(port));

      auto resolver_callback = [this, connection_id, port](asio::error_code ec, asio::ip::tcp::resolver::iterator itr)
      {
        if (!ec)
        {
          while (itr != asio::ip::tcp::resolver::iterator())
          {
            asio::ip::tcp::endpoint ep = *itr;
            
            if (ep.protocol() == ep.protocol().v4())
            {

              PrepareToConnect(connection_id, asio::ip::tcp::endpoint(ep.address(), port));
              return;
            }

            ++itr;
          }

          ConnectFailed(connection_id);
        }
        else
        {
          ConnectFailed(connection_id);
        }
      };

      m_Resolver.async_resolve(resolver_query, resolver_callback);
    }

    return connection_id;
  }

  bool StormSocketBackend::QueueOutgoingPacket(StormMessageWriter & writer, StormSocketConnectionId id)
  {
    return m_OutputQueue[id].Enqueue(writer, id.GetGen(), m_OutputQueueIncdices.get(), m_OutputQueueArray.get());
  }

  StormMessageWriter StormSocketBackend::CreateWriter(bool is_encrypted)
  {
    uint64_t prof = Profiling::StartProfiler();
    StormMessageWriter writer;
    writer.Init(&m_Allocator, &m_MessageSenders, is_encrypted, 0, 0);
    Profiling::EndProfiler(prof, ProfilerCategory::kCreatePacket);
    return writer;
  }

  StormHttpRequestWriter StormSocketBackend::CreateHttpRequestWriter(const char * method, const char * uri, const char * host)
  {
    auto header_writer = CreateWriter();
    auto body_writer = CreateWriter();

    StormHttpRequestWriter writer(method, uri, host, header_writer, body_writer);
    return writer;
  }

  StormHttpResponseWriter StormSocketBackend::CreateHttpResponseWriter(int response_code, char * response_phrase)
  {
    auto header_writer = CreateWriter();
    auto body_writer = CreateWriter();

    StormHttpResponseWriter writer(response_code, response_phrase, header_writer, body_writer);
    return writer;
  }

  void StormSocketBackend::ReferenceOutgoingHttpRequest(StormHttpRequestWriter & writer)
  {
    writer.m_HeaderWriter.m_PacketInfo->m_RefCount.fetch_add(1);
    writer.m_BodyWriter.m_PacketInfo->m_RefCount.fetch_add(1);
  }

  void StormSocketBackend::ReferenceOutgoingHttpResponse(StormHttpResponseWriter & writer)
  {
    writer.m_HeaderWriter.m_PacketInfo->m_RefCount.fetch_add(1);
    writer.m_BodyWriter.m_PacketInfo->m_RefCount.fetch_add(1);
  }

  void StormSocketBackend::FreeOutgoingHttpRequest(StormHttpRequestWriter & writer)
  {
    FreeOutgoingPacket(writer.m_HeaderWriter);
    FreeOutgoingPacket(writer.m_BodyWriter);
  }

  void StormSocketBackend::FreeOutgoingHttpResponse(StormHttpResponseWriter & writer)
  {
    FreeOutgoingPacket(writer.m_HeaderWriter);
    FreeOutgoingPacket(writer.m_BodyWriter);
  }

  bool StormSocketBackend::SendPacketToConnection(StormMessageWriter & writer, StormSocketConnectionId id)
  {
    if (writer.m_PacketInfo->m_TotalLength == 0)
    {
      return false;
    }

    auto & connection = GetConnection(id);
    if (connection.m_SlotGen != id.GetGen())
    {
      return false;
    }

    if (!ReservePacketSlot(id))
    {
      return false;
    }

    writer.m_PacketInfo->m_RefCount.fetch_add(1);
    if (QueueOutgoingPacket(writer, id) == false)
    {
      ReleasePacketSlot(id);
      writer.m_PacketInfo->m_RefCount.fetch_sub(1);
      return false;
    }

    connection.m_PacketsSent.fetch_add(1);
    SignalOutgoingSocket(id, StormSocketIOOperationType::SendPacket);
    return true;
  }

  void StormSocketBackend::SendPacketToConnectionBlocking(StormMessageWriter & writer, StormSocketConnectionId id)
  {
    while (!ReservePacketSlot(id))
    {
      std::this_thread::yield();
    }

    auto & connection = GetConnection(id);

    writer.m_PacketInfo->m_RefCount.fetch_add(1);
    while (QueueOutgoingPacket(writer, id) == false)
    {
      if (connection.m_SlotGen != id.GetGen())
      {
        ReleasePacketSlot(id);
        writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      if (connection.m_DisconnectFlags != 0)
      {
        ReleasePacketSlot(id);
        writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      std::this_thread::yield();
    }

    SignalOutgoingSocket(id, StormSocketIOOperationType::SendPacket);
  }

  void StormSocketBackend::SendHttpRequestToConnection(StormHttpRequestWriter & writer, StormSocketConnectionId id)
  {
    SendHttpToConnection(writer.m_HeaderWriter, writer.m_BodyWriter, id);
  }

  void StormSocketBackend::SendHttpResponseToConnection(StormHttpResponseWriter & writer, StormSocketConnectionId id)
  {
    SendHttpToConnection(writer.m_HeaderWriter, writer.m_BodyWriter, id);
  }

  void StormSocketBackend::SendHttpToConnection(StormMessageWriter & header_writer, StormMessageWriter & body_writer, StormSocketConnectionId id)
  {
    if (body_writer.GetLength() == 0)
    {
      SendPacketToConnectionBlocking(header_writer, id);
      return;
    }

    auto & connection = GetConnection(id);
    if (connection.m_SlotGen != id.GetGen())
    {
      return;
    }

    if (!ReservePacketSlot(id, 2))
    {
      return;
    }

    header_writer.m_PacketInfo->m_RefCount.fetch_add(1);
    body_writer.m_PacketInfo->m_RefCount.fetch_add(1);
    while (QueueOutgoingPacket(header_writer, id) == false)
    {
      if (connection.m_SlotGen != id.GetGen())
      {
        ReleasePacketSlot(id, 2);
        header_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        body_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      if (connection.m_DisconnectFlags != 0)
      {
        ReleasePacketSlot(id, 2);
        header_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        body_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      std::this_thread::yield();
    }

    while (QueueOutgoingPacket(body_writer, id) == false)
    {
      if (connection.m_SlotGen != id.GetGen())
      {
        ReleasePacketSlot(id);
        body_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      if (connection.m_DisconnectFlags != 0)
      {
        ReleasePacketSlot(id);
        body_writer.m_PacketInfo->m_RefCount.fetch_sub(1);
        return;
      }

      std::this_thread::yield();
    }

    connection.m_PacketsSent.fetch_add(2);

    int send_thread_index = id % m_NumSendThreads;

    StormSocketIOOperation op;
    op.m_ConnectionId = id;
    op.m_Type = StormSocketIOOperationType::SendPacket;
    op.m_Size = 0;

    while (m_SendQueue[send_thread_index].Enqueue(op, 0, m_SendQueueIncdices.get(), m_SendQueueArray.get()) == false)
    {
      std::this_thread::yield();
    }

    while (m_SendQueue[send_thread_index].Enqueue(op, 0, m_SendQueueIncdices.get(), m_SendQueueArray.get()) == false)
    {
      std::this_thread::yield();
    }

    m_SendThreadSemaphores[send_thread_index].Release(2);
  }

  void StormSocketBackend::FreeOutgoingPacket(StormMessageWriter & writer)
  {
    if (writer.m_PacketInfo->m_RefCount.fetch_sub(1) == 1)
    {
      ReleaseOutgoingPacket(writer);
    }
  }

  void StormSocketBackend::FinalizeConnection(StormSocketConnectionId id)
  {
    SetDisconnectFlag(id, StormSocketDisconnectFlags::kMainThread);
  }

  void StormSocketBackend::ForceDisconnect(StormSocketConnectionId id)
  {
    SetDisconnectFlag(id, StormSocketDisconnectFlags::kLocalClose);
  }

  bool StormSocketBackend::ConnectionIdValid(StormSocketConnectionId id)
  {
    auto & connection = GetConnection(id);
    return id.GetGen() == connection.m_SlotGen;
  }

  void StormSocketBackend::DiscardParserData(StormSocketConnectionId connection_id, int amount)
  {
    auto & connection = GetConnection(connection_id);
    if (connection.m_UnparsedDataLength < amount)
    {
      throw std::runtime_error("Read buffer underflow");
    }

    connection.m_ParseOffset += amount;
    while (connection.m_ParseOffset >= m_FixedBlockSize)
    {
      connection.m_ParseBlock = m_Allocator.GetNextBlock(connection.m_ParseBlock);
      connection.m_ParseOffset -= m_FixedBlockSize;
    }

    connection.m_UnparsedDataLength.fetch_sub(amount);
  }

  void StormSocketBackend::DiscardReaderData(StormSocketConnectionId connection_id, int amount)
  {
    auto & connection = GetConnection(connection_id);
    connection.m_RecvBuffer.DiscardData(amount);
  }

  bool StormSocketBackend::ReservePacketSlot(StormSocketConnectionId id, int amount)
  {
    auto & connection = GetConnection(id);

    while (true)
    {
      int pending_packets = connection.m_PendingPackets;
      if (pending_packets >= m_MaxPendingFrees - amount)
      {
        return false;
      }

      if (std::atomic_compare_exchange_weak(&connection.m_PendingPackets, &pending_packets, pending_packets + amount))
      {
        return true;
      }
    }
  }

  void StormSocketBackend::ReleasePacketSlot(StormSocketConnectionId id, int amount)
  {
    auto & connection = GetConnection(id);
    if (id.GetGen() == connection.m_SlotGen)
    {
      connection.m_PendingPackets.fetch_sub(amount);
    }
  }

  void StormSocketBackend::ReleaseOutgoingPacket(StormMessageWriter & writer)
  {
    StormFixedBlockHandle start_block = writer.m_PacketInfo->m_StartBlock;
    m_Allocator.FreeBlockChain(start_block, StormFixedBlockType::BlockMem);
    m_MessageSenders.FreeBlock(writer.m_PacketHandle, StormFixedBlockType::Sender);
  }

  void StormSocketBackend::SetSocketDisconnected(StormSocketConnectionId id)
  {
    auto & connection = GetConnection(id);

    while (true)
    {
      if (id.GetGen() != connection.m_SlotGen)
      {
        return;
      }

      StormSocketDisconnectFlags::Index cur_flags = (StormSocketDisconnectFlags::Index)connection.m_DisconnectFlags;
      if ((cur_flags & StormSocketDisconnectFlags::kSocket) != 0)
      {
        return;
      }

      int new_flags = cur_flags | StormSocketDisconnectFlags::kSocket | StormSocketDisconnectFlags::kLocalClose | StormSocketDisconnectFlags::kRemoteClose;
      if (std::atomic_compare_exchange_weak((std::atomic_int *)&connection.m_DisconnectFlags, (int *)&cur_flags, (int)new_flags))
      {
        // Tell the sending thread to flush the queue
        SignalOutgoingSocket(id, StormSocketIOOperationType::ClearQueue);

        connection.m_Frontend->QueueDisconnectEvent(id, connection.m_FrontendId);

        CheckDisconnectFlags(id, (StormSocketDisconnectFlags::Index)new_flags);
        return;
      }
    }
  }

  void StormSocketBackend::SignalCloseThread(StormSocketConnectionId id)
  {
    SetDisconnectFlag(id, StormSocketDisconnectFlags::kSignalClose);
  }

  void StormSocketBackend::SetDisconnectFlag(StormSocketConnectionId id, StormSocketDisconnectFlags::Index flags)
  {
    auto & connection = GetConnection(id);

    while (true)
    {
      if (id.GetGen() != connection.m_SlotGen)
      {
        return;
      }

      StormSocketDisconnectFlags::Index cur_flags = (StormSocketDisconnectFlags::Index)connection.m_DisconnectFlags;
      if ((cur_flags & flags) != 0)
      {
        return;
      }

      int new_flags = cur_flags | flags;
      if (std::atomic_compare_exchange_weak((std::atomic_int *)&connection.m_DisconnectFlags, (int *)&cur_flags, new_flags))
      {
        if (CheckDisconnectFlags(id, (StormSocketDisconnectFlags::Index)new_flags))
        {
          return;
        }

        if (flags == StormSocketDisconnectFlags::kLocalClose)
        {
          connection.m_Frontend->SendClosePacket(id, connection.m_FrontendId);
        }

        if ((flags == StormSocketDisconnectFlags::kLocalClose || flags == StormSocketDisconnectFlags::kRemoteClose) &&
          (new_flags & StormSocketDisconnectFlags::kSocket) == 0 &&
          (new_flags & StormSocketDisconnectFlags::kCloseFlags) == StormSocketDisconnectFlags::kCloseFlags)
        {
          SignalOutgoingSocket(id, StormSocketIOOperationType::Close);
        }

        if (flags == StormSocketDisconnectFlags::kSignalClose)
        {
          QueueCloseSocket(id);
          connection.m_FailedConnection = true;
        }

        return;
      }
    }
  }

  bool StormSocketBackend::CheckDisconnectFlags(StormSocketConnectionId id, StormSocketDisconnectFlags::Index new_flags)
  {
    auto & connection = GetConnection(id);
    if ((new_flags & StormSocketDisconnectFlags::kAllFlags) == StormSocketDisconnectFlags::kAllFlags)
    {
#ifdef USE_MBED
      FreeOutgoingPacket(connection.m_EncryptWriter);
#endif

      // Free any pent up packets
      FreeConnectionResources(id);
      connection.m_Frontend->CleanupConnection(id, connection.m_FrontendId);
      connection.m_Frontend->FreeFrontendId(connection.m_FrontendId);
      connection.m_Frontend->DisassociateConnectionId(id);

      // Free the recv buffer
      connection.m_RecvBuffer.FreeBuffers();
      connection.m_DecryptBuffer.FreeBuffers();

      connection.m_SlotGen = (connection.m_SlotGen + 1) & 0xFF;

      FreeConnectionSlot(id);
      return true;
    }

    return false;
  }

  StormSocketConnectionId StormSocketBackend::AllocateConnection(StormSocketFrontend * frontend, uint32_t remote_ip, uint16_t remote_port, bool for_connect, const void * init_data)
  {
    auto frontend_id = frontend->AllocateFrontendId();
    if (frontend_id == InvalidFrontendId)
    {
      return StormSocketConnectionId::InvalidConnectionId;
    }

    for (int index = 0; index < m_MaxConnections; index++)
    {
      auto & connection = GetConnection(index);
      if (connection.m_Used.test_and_set() == false)
      {
        // Set up the connection
        connection.m_DecryptBuffer = StormSocketBuffer(&m_Allocator, m_FixedBlockSize);
        connection.m_RecvBuffer = StormSocketBuffer(&m_Allocator, m_FixedBlockSize);
        connection.m_ParseBlock = InvalidBlockHandle;
        connection.m_PendingSendBlock = InvalidBlockHandle;
        connection.m_UnparsedDataLength = 0;
        connection.m_ParseOffset = 0;
        connection.m_ReadOffset = 0;
        connection.m_RemoteIP = remote_ip;
        connection.m_RemotePort = remote_port;
        connection.m_PendingPackets = 0;
        connection.m_PendingRemainingData = 0;
        connection.m_PendingFreeData = 0;
        connection.m_DisconnectFlags = 0;

        connection.m_SSLContext = SSLContext();
        connection.m_RecvCriticalSection = 0;

        connection.m_PacketsRecved = 0;
        connection.m_PacketsSent = 0;
        connection.m_FailedConnection = false;

        connection.m_RecvBuffer.InitBuffers();
#ifdef USE_MBED
        connection.m_EncryptWriter = CreateWriter(true);
#endif

        auto connection_id = StormSocketConnectionId(index, connection.m_SlotGen);
        connection.m_Frontend = frontend;
        connection.m_FrontendId = frontend_id;

        frontend->InitConnection(connection_id, frontend_id, init_data);

        if (for_connect == false)
        {
          connection.m_DisconnectFlags |= StormSocketDisconnectFlags::kConnectFinished;
          frontend->QueueConnectEvent(connection_id, connection.m_FrontendId, remote_ip, remote_port);
        }

        frontend->AssociateConnectionId(connection_id);

        return connection_id;
      }
    }

    frontend->FreeFrontendId(frontend_id);

    return StormSocketConnectionId::InvalidConnectionId;
  }

  void StormSocketBackend::FreeConnectionSlot(StormSocketConnectionId id)
  {
    auto & connection = GetConnection(id);
    connection.m_Used.clear();
  }

  void StormSocketBackend::PrepareToAccept(StormSocketBackendAcceptorId acceptor_id)
  {
    std::lock_guard<std::mutex> guard(m_AcceptorLock);
    auto acceptor_itr = m_Acceptors.find(acceptor_id);
    if (acceptor_itr == m_Acceptors.end())
    {
      return;
    }

    auto & acceptor = acceptor_itr->second;

    auto accept_callback = [this, acceptor_id](const asio::error_code & error) 
    { 
      AcceptNewConnection(error, acceptor_id); 
      PrepareToAccept(acceptor_id); 
    };

    acceptor.m_Acceptor.async_accept(acceptor.m_AcceptSocket, acceptor.m_AcceptEndpoint, accept_callback);
  }

  void StormSocketBackend::AcceptNewConnection(const asio::error_code & error, StormSocketBackendAcceptorId acceptor_id)
  {
    std::unique_lock<std::mutex> guard(m_AcceptorLock);

    auto acceptor_itr = m_Acceptors.find(acceptor_id);
    if (acceptor_itr == m_Acceptors.end())
    {
      return;
    }

    auto & acceptor = acceptor_itr->second;

    acceptor.m_AcceptSocket.set_option(asio::ip::tcp::no_delay(true));
    acceptor.m_AcceptSocket.set_option(asio::socket_base::linger(1, 1));
    acceptor.m_AcceptSocket.non_blocking(true);

    StormSocketConnectionId connection_id = AllocateConnection(acceptor.m_Frontend, 
      acceptor.m_AcceptEndpoint.address().to_v4().to_ulong(), acceptor.m_AcceptEndpoint.port(), false, nullptr);

    if (connection_id == StormSocketConnectionId::InvalidConnectionId)
    {
      acceptor.m_AcceptSocket.close();
      acceptor.m_AcceptSocket = asio::ip::tcp::socket(m_IOService);
      return;
    }

    m_ClientSockets[connection_id].emplace(std::move(acceptor.m_AcceptSocket));

    auto & connection = GetConnection(connection_id);

#ifdef USE_MBED
    if (acceptor.m_Frontend->UseSSL(connection_id, connection.m_FrontendId))
    {

      auto ssl_config = acceptor.m_Frontend->GetSSLConfig();

      mbedtls_ssl_init(&connection.m_SSLContext.m_SSLContext);
      mbedtls_ssl_setup(&connection.m_SSLContext.m_SSLContext, ssl_config);

      connection.m_DecryptBuffer.InitBuffers();

      auto send_callback = [](void * ctx, const unsigned char * data, size_t size) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        connection->m_EncryptWriter.WriteByteBlock(data, 0, size);
        return (int)size;
      };

      auto recv_callback = [](void * ctx, unsigned char * data, size_t size) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        if (connection->m_DecryptBuffer.m_DataAvail == 0)
        {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }

        void * block_start = connection->m_DecryptBuffer.m_Allocator->ResolveHandle(connection->m_DecryptBuffer.m_BlockStart);
        block_start = Marshal::MemOffset(block_start, connection->m_DecryptBuffer.m_ReadOffset);

        int mem_avail = connection->m_DecryptBuffer.m_Allocator->GetBlockSize() - connection->m_DecryptBuffer.m_ReadOffset;
        mem_avail = std::min(mem_avail, (int)connection->m_DecryptBuffer.m_DataAvail);
        mem_avail = std::min(mem_avail, (int)size);

        memcpy(data, block_start, mem_avail);
        return mem_avail;
      };

      auto recv_timeout_callback = [](void * ctx, unsigned char * data, size_t size, uint32_t timeout) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        if (connection->m_DecryptBuffer.m_DataAvail == 0)
        {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }

        void * block_start = connection->m_DecryptBuffer.m_Allocator->ResolveHandle(connection->m_DecryptBuffer.m_BlockStart);
        block_start = Marshal::MemOffset(block_start, connection->m_DecryptBuffer.m_ReadOffset);

        int mem_avail = connection->m_DecryptBuffer.m_Allocator->GetBlockSize() - connection->m_DecryptBuffer.m_ReadOffset;
        mem_avail = std::min(mem_avail, (int)connection->m_DecryptBuffer.m_DataAvail);
        mem_avail = std::min(mem_avail, (int)size);

        memcpy(data, block_start, mem_avail);
        connection->m_DecryptBuffer.DiscardData(mem_avail);
        return mem_avail;
      };

      mbedtls_ssl_set_bio(&connection.m_SSLContext.m_SSLContext,
        &connection,
        send_callback,
        recv_callback,
        recv_timeout_callback);

    }
    else
#endif
    {
      connection.m_Frontend->ConnectionEstablishComplete(connection_id, connection.m_FrontendId);
    }

    guard.unlock();
    PrepareToRecv(connection_id);
  }

  void StormSocketBackend::PrepareToConnect(StormSocketConnectionId id, asio::ip::tcp::endpoint endpoint)
  {
    auto & connection = GetConnection(id);
    if ((connection.m_DisconnectFlags & StormSocketDisconnectFlags::kAllFlags) != 0)
    {
      SetDisconnectFlag(id, StormSocketDisconnectFlags::kConnectFinished);
      return;
    }

    connection.m_RemoteIP = endpoint.address().to_v4().to_ulong();

    auto connect_callback = [this, id](asio::error_code ec)
    {
      if (!ec)
      {
        FinalizeSteamValidation(id);
      }
      else
      {
        ConnectFailed(id);
      }
    };

    m_ClientSockets[id]->async_connect(endpoint, connect_callback);
  }

  void StormSocketBackend::FinalizeSteamValidation(StormSocketConnectionId id)
  {
    auto & connection = GetConnection(id);

#ifdef USE_MBED
    if (connection.m_Frontend->UseSSL(id, connection.m_FrontendId))
    {
      auto ssl_config = connection.m_Frontend->GetSSLConfig();

      mbedtls_ssl_init(&connection.m_SSLContext.m_SSLContext);
      mbedtls_ssl_setup(&connection.m_SSLContext.m_SSLContext, ssl_config);

      connection.m_DecryptBuffer.InitBuffers();

      auto send_callback = [](void * ctx, const unsigned char * data, size_t size) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        connection->m_EncryptWriter.WriteByteBlock(data, 0, size);
        return (int)size;
      };

      auto recv_callback = [](void * ctx, unsigned char * data, size_t size) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        if (connection->m_DecryptBuffer.m_DataAvail == 0)
        {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }

        void * block_start = connection->m_DecryptBuffer.m_Allocator->ResolveHandle(connection->m_DecryptBuffer.m_BlockStart);
        block_start = Marshal::MemOffset(block_start, connection->m_DecryptBuffer.m_ReadOffset);

        int mem_avail = connection->m_DecryptBuffer.m_Allocator->GetBlockSize() - connection->m_DecryptBuffer.m_ReadOffset;
        mem_avail = std::min(mem_avail, (int)connection->m_DecryptBuffer.m_DataAvail);
        mem_avail = std::min(mem_avail, (int)size);

        memcpy(data, block_start, mem_avail);
        return mem_avail;
      };

      auto recv_timeout_callback = [](void * ctx, unsigned char * data, size_t size, uint32_t timeout) -> int
      {
        StormSocketConnectionBase * connection = (StormSocketConnectionBase *)ctx;
        if (connection->m_DecryptBuffer.m_DataAvail == 0)
        {
          return MBEDTLS_ERR_SSL_WANT_READ;
        }

        void * block_start = connection->m_DecryptBuffer.m_Allocator->ResolveHandle(connection->m_DecryptBuffer.m_BlockStart);
        block_start = Marshal::MemOffset(block_start, connection->m_DecryptBuffer.m_ReadOffset);

        int mem_avail = connection->m_DecryptBuffer.m_Allocator->GetBlockSize() - connection->m_DecryptBuffer.m_ReadOffset;
        mem_avail = std::min(mem_avail, (int)connection->m_DecryptBuffer.m_DataAvail);
        mem_avail = std::min(mem_avail, (int)size);

        memcpy(data, block_start, mem_avail);
        connection->m_DecryptBuffer.DiscardData(mem_avail);
        return mem_avail;
      };

      mbedtls_ssl_set_bio(&connection.m_SSLContext.m_SSLContext,
        &connection,
        send_callback,
        recv_callback,
        recv_timeout_callback);

      //mbedtls_debug_set_threshold(5);
      int ec = mbedtls_ssl_handshake(&connection.m_SSLContext.m_SSLContext);

      char error_str[1024];
      mbedtls_strerror(ec, error_str, sizeof(error_str));

      if (connection.m_EncryptWriter.m_PacketInfo->m_TotalLength > 0)
      {
        SendPacketToConnection(connection.m_EncryptWriter, id);
        FreeOutgoingPacket(connection.m_EncryptWriter);
        connection.m_EncryptWriter = CreateWriter(true);
      }
    }
    else
#endif
    {
      connection.m_Frontend->ConnectionEstablishComplete(id, connection.m_FrontendId);
    }

    connection.m_Frontend->QueueConnectEvent(id, connection.m_FrontendId, connection.m_RemoteIP, connection.m_RemotePort);

    PrepareToRecv(id);
    SetDisconnectFlag(id, StormSocketDisconnectFlags::kConnectFinished);
  }

  void StormSocketBackend::ConnectFailed(StormSocketConnectionId id)
  {
    SetSocketDisconnected(id);
    SetDisconnectFlag(id, StormSocketDisconnectFlags::kConnectFinished);
  }

  void StormSocketBackend::ProcessNewData(StormSocketConnectionId connection_id, const asio::error_code & error, std::size_t bytes_received)
  {
    auto & connection = GetConnection(connection_id);

    if (!error)
    {
#ifdef USE_MBED
      if (connection.m_Frontend->UseSSL(connection_id, connection.m_FrontendId))
      {
        connection.m_DecryptBuffer.GotData((int)bytes_received);
        while (connection.m_SSLContext.m_SSLHandshakeComplete == false)
        {
          int ec = mbedtls_ssl_handshake(&connection.m_SSLContext.m_SSLContext);

          char error_str[1024];
          mbedtls_strerror(ec, error_str, sizeof(error_str));

          if (connection.m_EncryptWriter.m_PacketInfo->m_TotalLength > 0)
          {
            SendPacketToConnection(connection.m_EncryptWriter, connection_id);
            FreeOutgoingPacket(connection.m_EncryptWriter);
            connection.m_EncryptWriter = CreateWriter(true);
          }

          if (ec == 0)
          {
            connection.m_SSLContext.m_SSLHandshakeComplete = true;
            connection.m_Frontend->ConnectionEstablishComplete(connection_id, connection.m_FrontendId);
          }
          else if (ec != MBEDTLS_ERR_SSL_WANT_READ)
          {
            SetSocketDisconnected(connection_id);
            SetDisconnectFlag(connection_id, StormSocketDisconnectFlags::kRecvThread);
            return;
          }
          else
          {
            break;
          }
        }
      }
      else
#endif
      {
        // Data just goes directly into the recv buffer
        connection.m_UnparsedDataLength.fetch_add((int)bytes_received);
        connection.m_RecvBuffer.GotData((int)bytes_received);
      }

      PrepareToRecv(connection_id);

      uint64_t prof = Profiling::StartProfiler();
      TryProcessReceivedData(connection_id);
      Profiling::EndProfiler(prof, ProfilerCategory::kProcData);
    }
    else
    {
      SetSocketDisconnected(connection_id);
      SetDisconnectFlag(connection_id, StormSocketDisconnectFlags::kRecvThread);
    }
  }


  void StormSocketBackend::TryProcessReceivedData(StormSocketConnectionId connection_id)
  {
    if (ProcessReceivedData(connection_id) == false)
    {
      auto recheck_callback = [=]() 
      {
        TryProcessReceivedData(connection_id);
      };

      ProfileScope prof(ProfilerCategory::kRepost);
      m_IOService.post(recheck_callback);
    }
  }

  void StormSocketBackend::SignalOutgoingSocket(StormSocketConnectionId id, StormSocketIOOperationType::Index type, std::size_t size)
  {
    int send_thread_index = id % m_NumSendThreads;

    StormSocketIOOperation op;
    op.m_ConnectionId = id;
    op.m_Type = type;
    op.m_Size = (int)size;

    while (m_SendQueue[send_thread_index].Enqueue(op, 0, m_SendQueueIncdices.get(), m_SendQueueArray.get()) == false)
    {
      std::this_thread::yield();
    }

    m_SendThreadSemaphores[send_thread_index].Release();
  }

  bool StormSocketBackend::ProcessReceivedData(StormSocketConnectionId connection_id)
  {
    auto & connection = GetConnection(connection_id);

    // Lock the receiver
    int old_val = 0;
    if (std::atomic_compare_exchange_weak(&connection.m_RecvCriticalSection, &old_val, 1) == false)
    {
      return false;
    }

#ifdef USE_MBED
    if (connection.m_Frontend->UseSSL(connection_id, connection.m_FrontendId))
    {
      auto prof = ProfileScope(ProfilerCategory::kSSLDecrypt);
      while (true)
      {
        void * block_mem = connection.m_RecvBuffer.m_Allocator->ResolveHandle(connection.m_RecvBuffer.m_BlockCur);
        block_mem = Marshal::MemOffset(block_mem, connection.m_RecvBuffer.m_WriteOffset);
        int mem_avail = connection.m_RecvBuffer.m_Allocator->GetBlockSize() - connection.m_RecvBuffer.m_WriteOffset;

        int ret = mbedtls_ssl_read(&connection.m_SSLContext.m_SSLContext, (uint8_t *)block_mem, mem_avail);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          break;
        }

        if (ret < 0)
        {
          char error_str[1024];
          mbedtls_strerror(ret, error_str, sizeof(error_str));

          SetSocketDisconnected(connection_id);
          SetDisconnectFlag(connection_id, StormSocketDisconnectFlags::kRecvThread);
          connection.m_RecvCriticalSection.store(0);
          return true;
        }

        connection.m_UnparsedDataLength.fetch_add(ret);
        connection.m_RecvBuffer.GotData(ret);
      }
    }
#endif

    bool success = connection.m_Frontend->ProcessData(connection_id, connection.m_FrontendId);

    connection.m_RecvCriticalSection.store(0);
    return success;
  }

  void StormSocketBackend::PrepareToRecv(StormSocketConnectionId connection_id)
  {
    auto & connection = GetConnection(connection_id);

#ifdef USE_MBED
    StormSocketBuffer * buffer = connection.m_Frontend->UseSSL(connection_id, connection.m_FrontendId) ? &connection.m_DecryptBuffer : &connection.m_RecvBuffer;
#else
    StormSocketBuffer * buffer = &connection.m_RecvBuffer
#endif

    void * buffer_start =
      Marshal::MemOffset(m_Allocator.ResolveHandle(buffer->m_BlockCur), buffer->m_WriteOffset);

    std::array<asio::mutable_buffer, 2> buffer_set = 
    {
      asio::buffer(buffer_start, m_FixedBlockSize - buffer->m_WriteOffset),
      asio::buffer(m_Allocator.ResolveHandle(buffer->m_BlockNext), m_FixedBlockSize)
    };

    auto recv_callback = [=](const asio::error_code & error, size_t bytes_received) { ProcessNewData(connection_id, error, bytes_received); };
    m_ClientSockets[connection_id]->async_read_some(buffer_set, recv_callback);
  }

  void StormSocketBackend::IOThreadMain()
  {
    while (m_ThreadStopRequested == false)
    {
      m_IOService.run();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void StormSocketBackend::SetBufferSet(SendBuffer & buffer_set, int buffer_index, const void * ptr, int length)
  {
    buffer_set[buffer_index] = asio::buffer(ptr, length);
  }

  int StormSocketBackend::FillBufferSet(SendBuffer & buffer_set, int & cur_buffer, int pending_data, StormMessageWriter & writer, int send_offset, StormFixedBlockHandle & send_block)
  {
    StormFixedBlockHandle block_handle = send_block;
    send_block = InvalidBlockHandle;
    int header_offset = send_offset;

    while (pending_data > 0 && cur_buffer < kBufferSetCount && block_handle != InvalidBlockHandle)
    {
      int potential_data_in_block = m_FixedBlockSize - header_offset - (writer.m_ReservedHeaderLength + writer.m_ReservedTrailerLength);
      int set_len = std::min(pending_data, potential_data_in_block);
      int data_start = writer.m_ReservedHeaderLength - writer.m_HeaderLength + header_offset;
      int data_length = writer.m_HeaderLength + set_len + writer.m_TrailerLength;

      void * block = m_Allocator.ResolveHandle(block_handle);

      SetBufferSet(buffer_set, cur_buffer, Marshal::MemOffset(block, data_start), data_length);
      block_handle = m_Allocator.GetNextBlock(block_handle);

      header_offset = 0;
      pending_data -= set_len;
      send_block = block_handle;

      cur_buffer++;
    }

    return pending_data;
  }

  void StormSocketBackend::SendThreadMain(int thread_index)
  {
    StormSocketIOOperation op;

    StormMessageWriter writer;

    while (m_ThreadStopRequested == false)
    {
      m_SendThreadSemaphores[thread_index].WaitOne(100);

      while (m_SendQueue[thread_index].TryDequeue(op, 0, m_SendQueueIncdices.get(), m_SendQueueArray.get()))
      {
        StormSocketConnectionId connection_id = op.m_ConnectionId;
        int connection_gen = connection_id.GetGen();
        auto & connection = GetConnection(connection_id);

        if (op.m_Type == StormSocketIOOperationType::FreePacket)
        {
          connection.m_PendingFreeData += op.m_Size;
          if (connection_gen != connection.m_SlotGen)
          {
            continue;
          }

          StormSocketFreeQueueElement free_elem;

          while (m_FreeQueue[connection_id].PeekTop(free_elem, connection_gen, m_FreeQueueIncdices.get(), m_FreeQueueArray.get(), 0))
          {
            int writer_len = free_elem.m_RequestWriter.m_PacketInfo->m_TotalLength;
            if (connection.m_PendingFreeData >= writer_len)
            {
              FreeOutgoingPacket(free_elem.m_RequestWriter);
              ReleasePacketSlot(connection_id);

              connection.m_PendingFreeData -= writer_len;
              m_FreeQueue[connection_id].Advance(connection_gen, m_FreeQueueIncdices.get(), m_FreeQueueArray.get());
            }
            else
            {
              break;
            }
          }
        }
        else if (op.m_Type == StormSocketIOOperationType::ClearQueue)
        {
          if (connection_gen != connection.m_SlotGen)
          {
            continue;
          }

          ReleaseSendQueue(connection_id, connection_gen);
          SetDisconnectFlag(connection_id, StormSocketDisconnectFlags::kSendThread);
          SignalCloseThread(connection_id);
        }
        else if (op.m_Type == StormSocketIOOperationType::Close)
        {
          if (connection_gen != connection.m_SlotGen)
          {
            continue;
          }

          SignalCloseThread(connection_id);
        }
        else if (op.m_Type == StormSocketIOOperationType::SendPacket)
        {
          SendBuffer buffer_set;
          if (m_OutputQueue[connection_id].PeekTop(writer, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), 0))
          {
            StormSocketFreeQueueElement free_queue_elem;

#ifdef USE_MBED
            if (writer.m_IsEncrypted == false && connection.m_Frontend->UseSSL(connection_id, connection.m_FrontendId))
            {
              StormMessageWriter encrypted = EncryptWriter(connection_id, writer);
              m_OutputQueue[connection_id].ReplaceTop(encrypted, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), 0);

              FreeOutgoingPacket(writer);

              writer = encrypted;
            }
#endif

            int buffer_count = 0;
            int packet_count = 0;
            int send_offset = 0;
            int total_send_length = 0;

            while (buffer_count < kBufferSetCount)
            {
              if (connection.m_PendingRemainingData == 0)
              {
                send_offset = writer.m_PacketInfo->m_SendOffset;
                connection.m_PendingRemainingData = writer.m_PacketInfo->m_TotalLength;
                connection.m_PendingSendBlock = writer.m_PacketInfo->m_StartBlock;
              }

              int remaining_data =
                FillBufferSet(buffer_set, buffer_count, connection.m_PendingRemainingData, writer, send_offset, connection.m_PendingSendBlock);

              total_send_length += connection.m_PendingRemainingData - remaining_data;
              connection.m_PendingRemainingData = remaining_data;

              packet_count++;

              if (m_OutputQueue[connection_id].PeekTop(writer, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), packet_count) == false)
              {
                break;
              }

#ifdef USE_MBED
              if (writer.m_IsEncrypted == false && connection.m_Frontend->UseSSL(connection_id, connection.m_FrontendId))
              {
                StormMessageWriter encrypted = EncryptWriter(connection_id, writer);
                m_OutputQueue[connection_id].ReplaceTop(encrypted, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), packet_count);

                FreeOutgoingPacket(writer);

                writer = encrypted;
              }
#endif
            }

            int advance_count;

            // If we sent the max blocks worth of data and still have shit to send...
            if (connection.m_PendingRemainingData > 0)
            {
              advance_count = packet_count - 1;
              // Free every writer that got written to except for the last one
              for (int index = 0; index < packet_count - 1; index++)
              {
                m_OutputQueue[connection_id].PeekTop(writer, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), index);
                free_queue_elem.m_RequestWriter = writer;

                if (m_FreeQueue[connection_id].Enqueue(free_queue_elem, connection_gen, m_FreeQueueIncdices.get(), m_FreeQueueArray.get()) == false)
                {
                  throw std::runtime_error("Free queue ran out of space");
                }
              }
              // Requeue up this operation so that we don't block out the number of writers
              if (m_SendQueue[thread_index].Enqueue(op, 0, m_SendQueueIncdices.get(), m_SendQueueArray.get()) == false)
              {
                throw std::runtime_error("Send queue ran out of space");
              }
            }
            else
            {
              advance_count = packet_count;
              // Just free everything
              for (int index = 0; index < packet_count; index++)
              {
                m_OutputQueue[connection_id].PeekTop(writer, connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get(), index);
                free_queue_elem.m_RequestWriter = writer;

                if (m_FreeQueue[connection_id].Enqueue(free_queue_elem, connection_gen, m_FreeQueueIncdices.get(), m_FreeQueueArray.get()) == false)
                {
                  throw std::runtime_error("Free queue ran out of space");
                }
              }
            }

            uint64_t prof = Profiling::StartProfiler();
            auto send_callback = [=](const asio::error_code & error, std::size_t bytes_transfered)
            {
              if (!error)
              {
                SignalOutgoingSocket(connection_id, StormSocketIOOperationType::FreePacket, bytes_transfered);
              }
              else
              {
                SetSocketDisconnected(connection_id);
              }
            };

            m_ClientSockets[connection_id]->async_send(buffer_set, send_callback);

            Profiling::EndProfiler(prof, ProfilerCategory::kSend);

            for (int index = 0; index < advance_count; index++)
            {
              m_OutputQueue[connection_id].Advance(connection_gen, m_OutputQueueIncdices.get(), m_OutputQueueArray.get());
            }
          }
        }
      }
    }
  }

  void StormSocketBackend::ReleaseSendQueue(StormSocketConnectionId connection_id, int connection_gen)
  {
    StormMessageWriter writer;
    // Lock the queue so that nothing else can put packets into it
    m_OutputQueue[connection_id].Lock(connection_gen + 1, m_OutputQueueIncdices.get(), m_OutputQueueArray.get());

    // Drain the remaining packets
    while (m_OutputQueue[connection_id].TryDequeue(writer, connection_gen + 1, m_OutputQueueIncdices.get(), m_OutputQueueArray.get()))
    {
      if (writer.m_PacketInfo != NULL)
      {
        FreeOutgoingPacket(writer);
      }
    }

    m_OutputQueue[connection_id].Reset(connection_gen + 1, m_OutputQueueIncdices.get(), m_OutputQueueArray.get());

    m_FreeQueue[connection_id].Lock(connection_gen + 1, m_FreeQueueIncdices.get(), m_FreeQueueArray.get());
    StormSocketFreeQueueElement free_elem;

    while (m_FreeQueue[connection_id].TryDequeue(free_elem, connection_gen + 1, m_FreeQueueIncdices.get(), m_FreeQueueArray.get()))
    {
      FreeOutgoingPacket(free_elem.m_RequestWriter);
    }

    m_FreeQueue[connection_id].Reset(connection_gen + 1, m_FreeQueueIncdices.get(), m_FreeQueueArray.get());
  }

  StormMessageWriter StormSocketBackend::EncryptWriter(StormSocketConnectionId connection_id, StormMessageWriter & writer)
  {
    auto prof = ProfileScope(ProfilerCategory::kSSLEncrypt);
#ifdef USE_MBED
    auto & connection = GetConnection(connection_id);
    StormFixedBlockHandle cur_block = writer.m_PacketInfo->m_StartBlock;

    int data_to_encrypt = writer.m_PacketInfo->m_TotalLength;

    int block_index = 0;
    while (cur_block != InvalidBlockHandle)
    {
      void * block_base = m_Allocator.ResolveHandle(cur_block);
      int start_offset = (block_index == 0 ? writer.m_PacketInfo->m_SendOffset : 0);
      int header_offset = writer.m_ReservedHeaderLength + start_offset;
      int block_size = writer.m_Allocator->GetBlockSize() - (writer.m_ReservedHeaderLength + writer.m_ReservedTrailerLength + start_offset);

      int data_size = std::min(data_to_encrypt, block_size);

      void * block_mem = Marshal::MemOffset(block_base, header_offset);
      int ec = mbedtls_ssl_write(&connection.m_SSLContext.m_SSLContext, (uint8_t *)block_mem, data_size);
      if (ec < 0)
      {
        throw std::runtime_error("Error encrypting packet");
      }

      data_to_encrypt -= data_size;

      cur_block = m_Allocator.GetNextBlock(cur_block);
      block_index++;
    }

    StormMessageWriter encrypted = connection.m_EncryptWriter;
    connection.m_EncryptWriter = CreateWriter(true);

    return encrypted;
#else

    return writer;

#endif
  }

  void StormSocketBackend::CloseSocketThread()
  {
    StormSocketConnectionId id;
    while (m_ThreadStopRequested == false)
    {
      m_CloseConnectionSemaphore.WaitOne();

      while (m_ClosingConnectionQueue.TryDequeue(id))
      {
        CloseSocket(id);
        SetSocketDisconnected(id);
        SetDisconnectFlag(id, StormSocketDisconnectFlags::kThreadClose);
      }
    }
  }

  void StormSocketBackend::QueueCloseSocket(StormSocketConnectionId id)
  {
    if (m_ClosingConnectionQueue.Enqueue(id))
    {
      m_CloseConnectionSemaphore.Release();
    }
    else
    {
      CloseSocket(id);
      SetSocketDisconnected(id);
      SetDisconnectFlag(id, StormSocketDisconnectFlags::kThreadClose);
    }
  }

  void StormSocketBackend::CloseSocket(StormSocketConnectionId id)
  {
    asio::error_code ec;
    m_ClientSockets[id]->close(ec);
  }

  void StormSocketBackend::FreeConnectionResources(StormSocketConnectionId id)
  {
#ifdef USE_MBED
    auto & connection = GetConnection(id);

    if (connection.m_Frontend->UseSSL(id, connection.m_FrontendId))
    {
      mbedtls_ssl_free(&connection.m_SSLContext.m_SSLContext);
    }
#endif

    m_ClientSockets[id]->close();
    m_ClientSockets[id] = std::experimental::optional<asio::ip::tcp::socket>();
  }
}
