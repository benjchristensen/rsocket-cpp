// Copyright 2004-present Facebook. All Rights Reserved.

#include "src/ReactiveSocket.h"

#include <folly/ExceptionWrapper.h>
#include <folly/Memory.h>
#include <folly/MoveWrapper.h>
#include "src/ClientResumeStatusCallback.h"
#include "src/ConnectionAutomaton.h"
#include "src/FrameTransport.h"
#include "src/ReactiveSocketSubscriberFactory.h"
#include "src/automata/ChannelRequester.h"
#include "src/automata/ChannelResponder.h"
#include "src/automata/RequestResponseRequester.h"
#include "src/automata/RequestResponseResponder.h"
#include "src/automata/StreamRequester.h"
#include "src/automata/StreamResponder.h"
#include "src/automata/SubscriptionRequester.h"
#include "src/automata/SubscriptionResponder.h"

namespace reactivesocket {

ReactiveSocket::~ReactiveSocket() {
  // Force connection closure, this will trigger terminal signals to be
  // delivered to all stream automata.
  close();

  // no more callbacks after the destructor is executed
  onConnectListeners_->clear();
  onDisconnectListeners_->clear();
  onCloseListeners_->clear();
}

ReactiveSocket::ReactiveSocket(
    bool isServer,
    std::shared_ptr<RequestHandlerBase> handler,
    Stats& stats,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer)
    : handler_(handler),
      keepaliveTimer_(std::move(keepaliveTimer)),
      connection_(new ConnectionAutomaton(
          [handler](
              ConnectionAutomaton& connection,
              StreamId streamId,
              std::unique_ptr<folly::IOBuf> serializedFrame) {
            ReactiveSocket::createResponder(
                handler, connection, streamId, std::move(serializedFrame));
          },
          std::make_shared<StreamState>(),
          std::bind(
              &ReactiveSocket::resumeListener,
              this,
              std::placeholders::_1),
          stats,
          keepaliveTimer_,
          isServer,
          executeListenersFunc(onConnectListeners_),
          executeListenersFunc(onDisconnectListeners_),
          executeListenersFunc(onCloseListeners_))),
      nextStreamId_(isServer ? 1 : 2) {}

std::unique_ptr<ReactiveSocket> ReactiveSocket::fromClientConnection(
    std::unique_ptr<DuplexConnection> connection,
    std::unique_ptr<RequestHandlerBase> handler,
    ConnectionSetupPayload setupPayload,
    Stats& stats,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer) {
  auto socket =
      disconnectedClient(std::move(handler), stats, std::move(keepaliveTimer));
  socket->connect(std::move(connection), std::move(setupPayload));
  return socket;
}

std::unique_ptr<ReactiveSocket> ReactiveSocket::disconnectedClient(
    std::unique_ptr<RequestHandlerBase> handler,
    Stats& stats,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer) {
  std::unique_ptr<ReactiveSocket> socket(new ReactiveSocket(
      false, std::move(handler), stats, std::move(keepaliveTimer)));
  return socket;
}

std::unique_ptr<ReactiveSocket> ReactiveSocket::fromServerConnection(
    std::unique_ptr<DuplexConnection> connection,
    std::unique_ptr<RequestHandlerBase> handler,
    Stats& stats,
    bool isResumable) {
  // TODO: isResumable should come as a flag on Setup frame and it should be
  // exposed to the application code. We should then remove this parameter
  std::unique_ptr<ReactiveSocket> socket(new ReactiveSocket(
      true,
      std::move(handler),
      stats,
      std::unique_ptr<KeepaliveTimer>(nullptr)));
  socket->connection_->setResumable(isResumable);
  socket->connection_->connect(
      FrameTransport::fromDuplexConnection(std::move(connection)));
  return socket;
}

std::shared_ptr<Subscriber<Payload>> ReactiveSocket::requestChannel(
    const std::shared_ptr<Subscriber<Payload>>& responseSink,
    folly::Executor& executor) {
  checkNotClosed();
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  ChannelRequester::Parameters params = {{connection_, streamId, handler_},
                                         executor};
  auto automaton = std::make_shared<ChannelRequester>(params);
  connection_->addStream(streamId, automaton);
  automaton->subscribe(responseSink);
  responseSink->onSubscribe(automaton);
  automaton->start();
  return automaton;
}

void ReactiveSocket::requestStream(
    Payload request,
    const std::shared_ptr<Subscriber<Payload>>& responseSink,
    folly::Executor& executor) {
  checkNotClosed();
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  StreamRequester::Parameters params = {{connection_, streamId, handler_},
                                        executor};
  auto automaton = std::make_shared<StreamRequester>(params);
  connection_->addStream(streamId, automaton);
  automaton->subscribe(responseSink);
  responseSink->onSubscribe(automaton);
  automaton->onNext(std::move(request));
  automaton->start();
}

void ReactiveSocket::requestSubscription(
    Payload request,
    const std::shared_ptr<Subscriber<Payload>>& responseSink,
    folly::Executor& executor) {
  checkNotClosed();
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  SubscriptionRequester::Parameters params = {{connection_, streamId, handler_},
                                              executor};
  auto automaton = std::make_shared<SubscriptionRequester>(params);
  connection_->addStream(streamId, automaton);
  automaton->subscribe(responseSink);
  responseSink->onSubscribe(automaton);
  automaton->onNext(std::move(request));
  automaton->start();
}

void ReactiveSocket::requestFireAndForget(Payload request) {
  checkNotClosed();
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  Frame_REQUEST_FNF frame(
      streamId, FrameFlags_EMPTY, std::move(std::move(request)));
  connection_->outputFrameOrEnqueue(frame.serializeOut());
}

void ReactiveSocket::requestResponse(
    Payload payload,
    const std::shared_ptr<Subscriber<Payload>>& responseSink,
    folly::Executor& executor) {
  checkNotClosed();
  // TODO(stupaq): handle any exceptions
  StreamId streamId = nextStreamId_;
  nextStreamId_ += 2;
  RequestResponseRequester::Parameters params = {
      {connection_, streamId, handler_}, executor};
  auto automaton = std::make_shared<RequestResponseRequester>(params);
  connection_->addStream(streamId, automaton);
  automaton->subscribe(responseSink);
  responseSink->onSubscribe(automaton);
  automaton->onNext(std::move(payload));
  automaton->start();
}

void ReactiveSocket::metadataPush(std::unique_ptr<folly::IOBuf> metadata) {
  checkNotClosed();
  connection_->outputFrameOrEnqueue(
      Frame_METADATA_PUSH(std::move(metadata)).serializeOut());
}

void ReactiveSocket::createResponder(
    std::shared_ptr<RequestHandlerBase> handler,
    ConnectionAutomaton& connection,
    StreamId streamId,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  auto type = FrameHeader::peekType(*serializedFrame);
  switch (type) {
    case FrameType::SETUP: {
      Frame_SETUP frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      if (frame.header_.flags_ & FrameFlags_LEASE) {
        // TODO(yschimke) We don't have the correct lease and wait logic above
        // yet
        LOG(WARNING) << "ignoring setup frame with lease";
        //          connectionOutput_.onNext(
        //              Frame_ERROR::badSetupFrame("leases not supported")
        //                  .serializeOut());
        //          disconnect();
      }

      auto streamState = handler->handleSetupPayload(ConnectionSetupPayload(
          std::move(frame.metadataMimeType_),
          std::move(frame.dataMimeType_),
          std::move(frame.payload_),
          false, // TODO: resumable flag should be received in SETUP frame
          frame.token_));

      connection.useStreamState(streamState);
      break;
    }
    case FrameType::REQUEST_CHANNEL: {
      Frame_REQUEST_CHANNEL frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      std::shared_ptr<ChannelResponder> automaton;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) {
            ChannelResponder::Parameters params = {
                {connection.shared_from_this(), streamId, handler},
                executor ? *executor : defaultExecutor()};
            automaton = std::make_shared<ChannelResponder>(params);
            connection.addStream(streamId, automaton);
            return automaton;
          });
      auto requestSink = handler->onRequestChannel(
          std::move(frame.payload_), streamId, subscriberFactory);
      if (!automaton) {
        auto subscriber = subscriberFactory.createSubscriber();
        subscriber->onSubscribe(std::make_shared<NullSubscription>());
        subscriber->onError(std::runtime_error("unhandled CHANNEL"));
      }
      if (automaton->subscribe(requestSink)) {
        // any calls from onSubscribe are queued until we start
        // TODO(lehecka): move the onSubscribe call to subscribe method
        requestSink->onSubscribe(automaton);
      }
      // processInitialFrame executes directly, it may cause to call request(n)
      // which may call back and it will be queued after the calls from
      // the onSubscribe method
      automaton->processInitialFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_STREAM: {
      Frame_REQUEST_STREAM frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      std::shared_ptr<StreamResponder> automaton;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) {
            StreamResponder::Parameters params = {
                {connection.shared_from_this(), streamId, handler},
                executor ? *executor : defaultExecutor()};
            automaton = std::make_shared<StreamResponder>(params);
            connection.addStream(streamId, automaton);
            return automaton;
          });
      handler->onRequestStream(
          std::move(frame.payload_), streamId, subscriberFactory);
      if (!automaton) {
        auto subscriber = subscriberFactory.createSubscriber();
        subscriber->onSubscribe(std::make_shared<NullSubscription>());
        subscriber->onError(std::runtime_error("unhandled STREAM"));
      }
      automaton->processInitialFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_SUB: {
      Frame_REQUEST_SUB frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      std::shared_ptr<SubscriptionResponder> automaton;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) {
            SubscriptionResponder::Parameters params = {
                {connection.shared_from_this(), streamId, handler},
                executor ? *executor : defaultExecutor()};
            automaton = std::make_shared<SubscriptionResponder>(params);
            connection.addStream(streamId, automaton);
            return automaton;
          });
      handler->onRequestSubscription(
          std::move(frame.payload_), streamId, subscriberFactory);
      if (!automaton) {
        auto subscriber = subscriberFactory.createSubscriber();
        subscriber->onSubscribe(std::make_shared<NullSubscription>());
        subscriber->onError(std::runtime_error("unhandled SUBSCRIPTION"));
      }
      automaton->processInitialFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_RESPONSE: {
      Frame_REQUEST_RESPONSE frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      std::shared_ptr<RequestResponseResponder> automaton;
      ReactiveSocketSubscriberFactory subscriberFactory(
          [&](folly::Executor* executor) {
            RequestResponseResponder::Parameters params = {
                {connection.shared_from_this(), streamId, handler},
                executor ? *executor : defaultExecutor()};
            automaton = std::make_shared<RequestResponseResponder>(params);
            connection.addStream(streamId, automaton);
            return automaton;
          });
      handler->onRequestResponse(
          std::move(frame.payload_), streamId, subscriberFactory);
      // we need to create a responder to at least close the stream
      if (!automaton) {
        auto subscriber = subscriberFactory.createSubscriber();
        subscriber->onSubscribe(std::make_shared<NullSubscription>());
        subscriber->onError(std::runtime_error("unhandled REQUEST/RESPONSE"));
      }
      automaton->processInitialFrame(std::move(frame));
      automaton->start();
      break;
    }
    case FrameType::REQUEST_FNF: {
      Frame_REQUEST_FNF frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      // no stream tracking is necessary
      handler->handleFireAndForgetRequest(std::move(frame.payload_), streamId);
      break;
    }
    case FrameType::METADATA_PUSH: {
      Frame_METADATA_PUSH frame;
      if (!connection.deserializeFrameOrError(
              frame, std::move(serializedFrame))) {
        return;
      }
      handler->handleMetadataPush(std::move(frame.metadata_));
      break;
    }
    // Other frames cannot start a stream.

    case FrameType::LEASE:
    case FrameType::KEEPALIVE:
    case FrameType::RESERVED:
    default:
      connection.closeWithError(Frame_ERROR::unexpectedFrame());
  }
}

std::shared_ptr<StreamState> ReactiveSocket::resumeListener(
    const ResumeIdentificationToken& token) {
  return handler_->handleResume(token);
}

void ReactiveSocket::connect(
    std::unique_ptr<DuplexConnection> connection,
    ConnectionSetupPayload setupPayload) {
  CHECK(connection);
  checkNotClosed();
  connection_->setResumable(setupPayload.resumable);

  uint32_t keepaliveTime = keepaliveTimer_
      ? keepaliveTimer_->keepaliveTime().count()
      : std::numeric_limits<uint32_t>::max();

  // TODO set correct version
  Frame_SETUP frame(
      FrameFlags_EMPTY,
      0,
      keepaliveTime,
      std::numeric_limits<uint32_t>::max(),
      // TODO: resumability,
      setupPayload.token,
      std::move(setupPayload.metadataMimeType),
      std::move(setupPayload.dataMimeType),
      std::move(setupPayload.payload));

  // TODO: when the server returns back that it doesn't support resumability, we
  // should retry without resumability

  // making sure we send setup frame first
  connection_->outputFrameOrEnqueue(frame.serializeOut());
  // then the rest of the cached frames will be sent
  connection_->connect(
      FrameTransport::fromDuplexConnection(std::move(connection)));

  if (keepaliveTimer_) {
    keepaliveTimer_->start(connection_);
  }
}

void ReactiveSocket::close() {
  // Stop scheduling keepalives since the socket is now closed, but may be
  // destructed later
  if (keepaliveTimer_) {
    keepaliveTimer_->stop();
  }
  if (connection_) {
    connection_->close();
    connection_ = nullptr;
  }
}

void ReactiveSocket::disconnect() {
  checkNotClosed();
  // Stop scheduling keepalives since the socket is now disconnected
  // TODO(lehecka): reenable keepaliveTimer on Resume_OK
  if (keepaliveTimer_) {
    keepaliveTimer_->stop();
  }
  connection_->disconnect();
}

void ReactiveSocket::onConnected(ReactiveSocketCallback listener) {
  checkNotClosed();
  CHECK(listener);
  onConnectListeners_->push_back(std::move(listener));
}

void ReactiveSocket::onDisconnected(ReactiveSocketCallback listener) {
  checkNotClosed();
  CHECK(listener);
  onDisconnectListeners_->push_back(std::move(listener));
}

void ReactiveSocket::onClosed(ReactiveSocketCallback listener) {
  checkNotClosed();
  CHECK(listener);
  onCloseListeners_->push_back(std::move(listener));
}

void ReactiveSocket::tryClientResume(
    const ResumeIdentificationToken& token,
    std::unique_ptr<DuplexConnection> newConnection,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback,
    bool /*closeReactiveSocketOnFailure*/) {
  checkNotClosed();
  connection_->reconnect(
      FrameTransport::fromDuplexConnection(std::move(newConnection)),
      std::move(resumeCallback));
  connection_->sendResume(token);

  // TODO: use closeReactiveSocketOnFailure
}

std::function<void()> ReactiveSocket::executeListenersFunc(
    std::shared_ptr<std::list<ReactiveSocketCallback>> listeners) {
  return [this, listeners]() {
    for (auto& listener : *listeners) {
      listener(*this);
    }
  };
}

void ReactiveSocket::checkNotClosed() const {
  CHECK(connection_) << "ReactiveSocket already closed";
}

} // reactivesocket
