#ifndef MANGOSSOCKET_H
#define MANGOSSOCKET_H

#include <ace/Basic_Types.h>
#include <ace/Synch_Traits.h>
#include <ace/Svc_Handler.h>
#include <ace/SOCK_Stream.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/SOCK_Connector.h>
#include <ace/Acceptor.h>
#include <ace/Connector.h>
#include <ace/Unbounded_Queue.h>
#include <ace/Message_Block.h>
#include <mutex>

#if !defined (ACE_LACKS_PRAGMA_ONCE)
#pragma once
#endif /* ACE_LACKS_PRAGMA_ONCE */

#include "Common.h"

class ACE_Message_Block;
class WorldPacket;
class WorldSession;


#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

struct ServerPktHeader
{
    uint16 size;
    uint16 cmd;
};

struct ClientPktHeader
{
    uint16 size;
    uint32 cmd;
};

#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

/// Handler that can communicate over stream sockets.
typedef ACE_Svc_Handler<ACE_SOCK_STREAM, ACE_NULL_SYNCH> WorldHandler;

/**
 * MangosSocket.
 *
 * This class is responsible for the communication with
 * remote clients.
 * Most methods return -1 on failure.
 * The class uses reference counting.
 *
 * For output the class uses one buffer (64K usually) and
 * a queue where it stores packet if there is no place on
 * the queue. The reason this is done, is because the server
 * does really a lot of small-size writes to it, and it doesn't
 * scale well to allocate memory for every. When something is
 * written to the output buffer the socket is not immediately
 * activated for output (again for the same reason), there
 * is 10ms celling (thats why there is Update() method).
 * This concept is similar to TCP_CORK, but TCP_CORK
 * uses 200ms celling. As result overhead generated by
 * sending packets from "producer" threads is minimal,
 * and doing a lot of writes with small size is tolerated.
 *
 * The calls to Update () method are managed by WorldSocketMgr
 * and ReactorRunnable.
 *
 * For input ,the class uses one 1024 bytes buffer on stack
 * to which it does recv() calls. And then received data is
 * distributed where its needed. 1024 matches pretty well the
 * traffic generated by client for now.
 *
 * The input/output do speculative reads/writes (AKA it tryes
 * to read all data available in the kernel buffer or tryes to
 * write everything available in userspace buffer),
 * which is ok for using with Level and Edge Triggered IO
 * notification.
 *
 */
template <typename SessionType, typename SocketName, typename Crypt>
class MangosSocket : protected WorldHandler
{
    public:
        /// Declare the acceptor for this class
        typedef ACE_Acceptor<SocketName, ACE_SOCK_ACCEPTOR> Acceptor;
        typedef ACE_Connector<SocketName,ACE_SOCK_CONNECTOR> Connector;
        /// Declare some friends
        friend class ACE_Acceptor<SocketName, ACE_SOCK_ACCEPTOR>;
        friend class ACE_Connector<SocketName, ACE_SOCK_CONNECTOR>;
        friend class ACE_NonBlocking_Connect_Handler<SocketName>;

        /// Mutex type used for various synchronizations.
        using LockType = std::mutex;
        typedef std::unique_lock<LockType> GuardType;

        /// Queue for storing packets for which there is no space.
        typedef ACE_Unbounded_Queue<WorldPacket*> PacketQueueT;

        /// Check if socket is closed.
        bool IsClosed() const { return closing_; }

        /// Close the socket.
        void CloseSocket (void);

        /// Get address of connected peer.
        const std::string& GetRemoteAddress () const { return m_Address; }

        /// Send A packet on the socket, this function is reentrant.
        /// @param pct packet to send
        /// @return -1 of failure
        int SendPacket (const WorldPacket& pct);

        /// Add reference to this object.
        long AddReference() { return static_cast<long>(add_reference()); }

        /// Remove reference to this object.
        long RemoveReference() { return static_cast<long>(remove_reference()); }

        void SetSession(SessionType* t) { m_Session = t; }
        void SetClientSocket() { m_isServerSocket = false; }
        /**
         * @brief returns true iif the socket is connected TO a client (ie we are the server)
         */
        bool IsServerSide() { return m_isServerSocket; }
    protected:
        /// things called by ACE framework.
        MangosSocket();
        virtual ~MangosSocket (void);

        /// process one incoming packet.
        /// @param new_pct received packet ,note that you need to delete it.
        int ProcessIncoming (WorldPacket* new_pct) { delete new_pct; return 0; }
        int OnSocketOpen() { return 0; }

        /// Called on open ,the void* is the acceptor.
        virtual int open (void *);

        /// Called on failures inside of the acceptor, don't call from your code.
        virtual int close (int);

        /// Called when we can read from the socket.
        virtual int handle_input (ACE_HANDLE = ACE_INVALID_HANDLE);

        /// Called when the socket can write.
        virtual int handle_output (ACE_HANDLE = ACE_INVALID_HANDLE);

        /// Called when connection is closed or error happens.
        virtual int handle_close (ACE_HANDLE = ACE_INVALID_HANDLE,
            ACE_Reactor_Mask = ACE_Event_Handler::ALL_EVENTS_MASK);

        /// Called by WorldSocketMgr/ReactorRunnable.
        int Update (void);

        /// Helper functions for processing incoming data.
        int handle_input_header (void);
        int handle_input_payload (void);
        int handle_input_missing_data (void);

        /// Help functions to mark/unmark the socket for output.
        /// @param g the guard is for m_OutBufferLock, the function will release it
        int cancel_wakeup_output (GuardType& g);
        int schedule_wakeup_output (GuardType& g);

        /// Try to write WorldPacket to m_OutBuffer ,return -1 if no space
        /// Need to be called with m_OutBufferLock lock held
        int iSendPacket (const WorldPacket& pct);

        /// Flush m_PacketQueue if there are packets in it
        /// Need to be called with m_OutBufferLock lock held
        /// @return true if it wrote to the buffer ( AKA you need
        /// to mark the socket for output ).
        bool iFlushPacketQueue ();

        /// Time in which the last ping was received
        ACE_Time_Value m_LastPingTime;

        /// Keep track of over-speed pings ,to prevent ping flood.
        uint32 m_OverSpeedPings;

        /// Address of the remote peer
        std::string m_Address;

        /// Class used for managing encryption of the headers
        Crypt m_Crypt;

        /// Mutex lock to protect m_Session
        LockType m_SessionLock;

        /// Session to which received packets are routed
        SessionType* m_Session;

        /// here are stored the fragments of the received data
        WorldPacket* m_RecvWPct;

        /// This block actually refers to m_RecvWPct contents,
        /// which allows easy and safe writing to it.
        /// It wont free memory when its deleted. m_RecvWPct takes care of freeing.
        ACE_Message_Block m_RecvPct;

        /// Fragment of the received header.
        ACE_Message_Block m_Header;

        /// Mutex for protecting output related data.
        LockType m_OutBufferLock;

        /// Buffer used for writing output.
        ACE_Message_Block *m_OutBuffer;

        /// Size of the m_OutBuffer.
        size_t m_OutBufferSize;

        /// Here are stored packets for which there was no space on m_OutBuffer,
        /// this allows not-to kick player if its buffer is overflowed.
        PacketQueueT m_PacketQueue;

        /// True if the socket is registered with the reactor for output
        bool m_OutActive;

        uint32 m_Seed;

        bool m_isServerSocket;
};

#endif // MANGOSSOCKET_H
