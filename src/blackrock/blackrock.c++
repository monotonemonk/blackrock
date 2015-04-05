// Sandstorm Blackrock
// Copyright (c) 2014 Sandstorm Development Group, Inc.
// All Rights Reserved

#include <kj/main.h>
#include <kj/async-io.h>
#include <capnp/rpc.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/serialize-packed.h>
#include <sandstorm/version.h>
#include <sandstorm/util.h>
#include <blackrock/machine.capnp.h>
#include "cluster-rpc.h"
#include "worker.h"
#include "fs-storage.h"

namespace blackrock {

static struct sockaddr_in ip4Wildcard() {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  return addr;
}

class MachineImpl: public Machine::Server {
  // TODO(security): For most become*() methods, we should probably actually spawn a child process.
  //   (But before we do that we probably need to implement Cap'n Proto Level 3.)

public:
  MachineImpl(kj::AsyncIoContext& ioContext): ioContext(ioContext) {}
  ~MachineImpl() {
    KJ_LOG(WARNING, "master disconnected");
  }

  kj::Promise<void> becomeStorage(BecomeStorageContext context) override {
    mkdir("/var", 0777);
    mkdir("/var/blackrock", 0777);
    mkdir("/var/blackrock/storage", 0777);

    StorageRootSet::Client storage = kj::heap<FilesystemStorage>(
        sandstorm::raiiOpen("/var/blackrock/storage", O_RDONLY | O_DIRECTORY | O_CLOEXEC),
        ioContext.unixEventPort, ioContext.lowLevelProvider->getTimer(), nullptr);
    // TODO(someday): restorers, both incoming and outgoing
    auto results = context.getResults();
    results.setStorageFactory(storage.getFactoryRequest().send().getFactory());
    results.setRootSet(kj::mv(storage));

    return kj::READY_NOW;
  }

  kj::Promise<void> becomeWorker(BecomeWorkerContext context) override {
    context.getResults().setWorker(kj::heap<WorkerImpl>(ioContext));
    return kj::READY_NOW;
  }

private:
  kj::AsyncIoContext& ioContext;
};

class MasterImpl: public Master::Server {
public:
  kj::Promise<void> addMachine(AddMachineContext context) override {
    KJ_DBG("slave connected");
    return kj::READY_NOW;
  }

private:
};

class Main {
public:
  Main(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock.")
        .addSubCommand("master", KJ_BIND_METHOD(*this, getMasterMain), "run as master node")
        .addSubCommand("slave", KJ_BIND_METHOD(*this, getSlaveMain), "run as slave node")
        .addSubCommand("grain", KJ_BIND_METHOD(*this, getSupervisorMain), "run a supervised grain")
        .build();
  }

  kj::MainFunc getMasterMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock master.")
        .callAfterParsing(KJ_BIND_METHOD(*this, runMaster))
        .build();
  }

  kj::MainFunc getSlaveMain() {
    return kj::MainBuilder(context, "Sandstorm Blackrock version " SANDSTORM_VERSION,
                           "Starts Blackrock slave, taking commands from the given master. "
                           "<master-path> is the base64-encoded serialized VatPath of the master.")
        .expectArg("<master-path>", KJ_BIND_METHOD(*this, runSlave))
        .build();
  }

  kj::MainFunc getSupervisorMain() {
    alternateMain = kj::heap<SupervisorMain>(context);
    return alternateMain->getMain();
  }

private:
  kj::ProcessContext& context;
  kj::Own<sandstorm::AbstractMain> alternateMain;

  bool runMaster() {
    auto ioContext = kj::setupAsyncIo();

    VatNetwork network(ioContext.provider->getNetwork(),
        ioContext.provider->getTimer(), ip4Wildcard());
    auto rpcSystem = capnp::makeRpcServer(network, kj::heap<MasterImpl>());

    // Print the master path.
    {
      capnp::MallocMessageBuilder message;
      message.setRoot(network.getSelf());

      byte bytes[256];
      kj::ArrayOutputStream stream(bytes);
      capnp::writePackedMessage(stream, message);

      auto text = sandstorm::base64Encode(stream.getArray(), false);
      context.warning(kj::str("master path: ", text));
    }

    // Loop forever handling messages.
    kj::NEVER_DONE.wait(ioContext.waitScope);
    KJ_UNREACHABLE;
  }

  bool runSlave(kj::StringPtr masterAddr) {
    auto ioContext = kj::setupAsyncIo();

    VatNetwork network(ioContext.provider->getNetwork(),
        ioContext.provider->getTimer(), ip4Wildcard());
    auto rpcSystem = capnp::makeRpcClient(network);

    // Decode the master path and connect to the master.
    {
      auto bytes = sandstorm::base64Decode(masterAddr);
      kj::ArrayInputStream stream(bytes);
      capnp::PackedMessageReader masterAddrReader(stream);
      auto masterPath = masterAddrReader.getRoot<VatPath>();

      auto master = rpcSystem.bootstrap(masterPath).castAs<Master>();
      auto request = master.addMachineRequest();
      request.setMachine(kj::heap<MachineImpl>(ioContext));
      request.send().wait(ioContext.waitScope);
    }

    // Loop forever handling messages.
    kj::NEVER_DONE.wait(ioContext.waitScope);
    KJ_UNREACHABLE;
  }
};

}  // namespace blackrock

KJ_MAIN(blackrock::Main)
