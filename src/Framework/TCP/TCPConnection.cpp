#include "TCPConnection.h"
#include "System/Events/EventLoop.h"

TCPConnection::TCPConnection()
{
	state = DISCONNECTED;
	next = NULL;
	connectTimeout.SetCallable(MFUNC(TCPConnection, OnConnectTimeout));
	writeQueue = new TCPWriteQueue(this);
}

TCPConnection::~TCPConnection()
{	
	Close();
}

void TCPConnection::Init(bool startRead)
{
	Log_Trace();
	
	assert(tcpread.active == false);
	assert(tcpwrite.active == false);

	readBuffer.Rewind();
	
	tcpwrite.SetFD(socket.fd);
	tcpwrite.SetOnComplete(MFUNC(TCPConnection, OnRead));
	tcpwrite.SetOnClose(MFUNC(TCPConnection, OnClose));

	AsyncRead(startRead);
}

void TCPConnection::InitConnected(bool startRead)
{
	Init(startRead);
	state = CONNECTED;
}

TCPWriteQueue* TCPConnection::GetWriteQueue()
{
	return writeQueue;
}

void TCPConnection::Connect(Endpoint &endpoint, unsigned timeout)
{
	Log_Trace("endpoint_ = %s", endpoint.ToString());

	bool ret;

	if (state != DISCONNECTED)
		return;

	Init(false);
	state = CONNECTING;

	socket.Create(Socket::TCP);
	socket.SetNonblocking();
	ret = socket.Connect(endpoint);

	tcpwrite.SetFD(socket.fd);
	tcpwrite.SetOnComplete(MFUNC(TCPConnection, OnConnect));
	tcpwrite.SetOnClose(MFUNC(TCPConnection, OnClose));
	tcpwrite.AsyncConnect();
	IOProcessor::Add(&tcpwrite);

	if (timeout > 0)
	{
		Log_Trace("starting timeout with %d", timeout);

		connectTimeout.SetDelay(timeout);
		EventLoop::Reset(&connectTimeout);
	}
}

void TCPConnection::OnWrite()
{
	Log_Trace("Written %d bytes", tcpwrite.buffer->GetLength());
	Log_Trace("Written: %.*s", P(tcpwrite.buffer));

	assert(writeQueue != NULL);
	writeQueue->OnNextWritten();
}

void TCPConnection::OnConnect()
{
	Log_Trace();

	socket.SetNodelay();
	
	state = CONNECTED;
	tcpwrite.onComplete = MFUNC(TCPConnection, OnWrite);
	
	EventLoop::Remove(&connectTimeout);
	OnWritePending();
}

void TCPConnection::OnConnectTimeout()
{
	Log_Trace();
}

void TCPConnection::AsyncRead(bool start)
{
	Log_Trace();
	
	tcpread.SetFD(socket.fd);
	tcpread.SetOnComplete(MFUNC(TCPConnection, OnRead));
	tcpread.SetOnClose(MFUNC(TCPConnection, OnClose));
	tcpread.buffer = &readBuffer;
	if (start)
		IOProcessor::Add(&tcpread);
	else
		Log_Trace("not posting read");
}

void TCPConnection::OnWritePending()
{
	Buffer* buffer;

	if (state == DISCONNECTED || tcpwrite.active)
		return;

	assert(writeQueue != NULL);
	buffer = writeQueue->GetNext();
	
	if (buffer == NULL)
		return;

	tcpwrite.SetBuffer(buffer);
	IOProcessor::Add(&tcpwrite);
}

void TCPConnection::Close()
{
	Log_Trace();

	EventLoop::Remove(&connectTimeout);

	IOProcessor::Remove(&tcpread);
	IOProcessor::Remove(&tcpwrite);

	socket.Close();
	state = DISCONNECTED;
	
	if (writeQueue)
		writeQueue->OnClose();
}
