#include "StarWorldServerThread.hpp"
#include "StarTickRateMonitor.hpp"
#include "StarNpc.hpp"
#include "StarRoot.hpp"
#include "StarLogging.hpp"
#include "StarAssets.hpp"
#include "StarPlayer.hpp"

namespace Star {

WorldServerThread::WorldServerThread(WorldServerPtr server, WorldId worldId)
  : Thread("WorldServerThread: " + printWorldId(worldId)),
    m_worldServer(move(server)),
    m_worldId(move(worldId)),
    m_stop(false),
    m_errorOccurred(false) {}

WorldServerThread::~WorldServerThread() {
  m_stop = true;
  join();

  RecursiveMutexLocker locker(m_mutex);
  for (auto clientId : m_worldServer->clientIds())
    removeClient(clientId);
}

WorldId WorldServerThread::worldId() const {
  return m_worldId;
}

void WorldServerThread::start() {
  m_stop = false;
  m_errorOccurred = false;
  Thread::start();
}

void WorldServerThread::stop() {
  m_stop = true;
  Thread::join();
}

void WorldServerThread::setPause(shared_ptr<const atomic<bool>> pause) {
  m_pause = pause;
}

bool WorldServerThread::serverErrorOccurred() {
  return m_errorOccurred;
}

bool WorldServerThread::spawnTargetValid(SpawnTarget const& spawnTarget) {
  try {
    RecursiveMutexLocker locker(m_mutex);
    return m_worldServer->spawnTargetValid(spawnTarget);
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

bool WorldServerThread::addClient(ConnectionId clientId, SpawnTarget const& spawnTarget, bool isLocal) {
  try {
    RecursiveMutexLocker locker(m_mutex);
    if (m_worldServer->addClient(clientId, spawnTarget, isLocal)) {
      m_clients.add(clientId);
      return true;
    }

    return false;
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return false;
  }
}

List<PacketPtr> WorldServerThread::removeClient(ConnectionId clientId) {
  RecursiveMutexLocker locker(m_mutex);
  if (!m_clients.contains(clientId))
    return {};

  RecursiveMutexLocker queueLocker(m_queueMutex);

  List<PacketPtr> outgoingPackets;
  try {
    auto incomingPackets = take(m_incomingPacketQueue[clientId]);
    if (m_worldServer->hasClient(clientId))
      m_worldServer->handleIncomingPackets(clientId, move(incomingPackets));

    outgoingPackets = take(m_outgoingPacketQueue[clientId]);
    if (m_worldServer->hasClient(clientId))
      outgoingPackets.appendAll(m_worldServer->removeClient(clientId));

  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }

  m_clients.remove(clientId);
  m_incomingPacketQueue.remove(clientId);
  m_outgoingPacketQueue.remove(clientId);
  return outgoingPackets;
}

List<ConnectionId> WorldServerThread::clients() const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.values();
}

bool WorldServerThread::hasClient(ConnectionId clientId) const {
  RecursiveMutexLocker locker(m_mutex);
  return m_clients.contains(clientId);
}

List<ConnectionId> WorldServerThread::erroredClients() const {
  RecursiveMutexLocker locker(m_mutex);
  auto unerroredClients = HashSet<ConnectionId>::from(m_worldServer->clientIds());
  return m_clients.difference(unerroredClients).values();
}

void WorldServerThread::pushIncomingPackets(ConnectionId clientId, List<PacketPtr> packets) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  m_incomingPacketQueue[clientId].appendAll(move(packets));
}

List<PacketPtr> WorldServerThread::pullOutgoingPackets(ConnectionId clientId) {
  RecursiveMutexLocker queueLocker(m_queueMutex);
  return take(m_outgoingPacketQueue[clientId]);
}

Maybe<Vec2F> WorldServerThread::playerRevivePosition(ConnectionId clientId) const {
  try {
    RecursiveMutexLocker locker(m_mutex);
    if (auto player = m_worldServer->clientPlayer(clientId))
      return player->position() + player->feetOffset();
    return {};
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

Maybe<pair<String, String>> WorldServerThread::pullNewPlanetType() {
  try {
    RecursiveMutexLocker locker(m_mutex);
    return m_worldServer->pullNewPlanetType();
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::executeAction(WorldServerAction action) {
  RecursiveMutexLocker locker(m_mutex);
  action(this, m_worldServer.get());
}

void WorldServerThread::setUpdateAction(WorldServerAction updateAction) {
  RecursiveMutexLocker locker(m_mutex);
  m_updateAction = updateAction;
}

WorldChunks WorldServerThread::readChunks() {
  try {
    RecursiveMutexLocker locker(m_mutex);
    return m_worldServer->readChunks();
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
    return {};
  }
}

void WorldServerThread::run() {
  try {
    auto& root = Root::singleton();
    double updateMeasureWindow = root.assets()->json("/universe_server.config:updateMeasureWindow").toDouble();
    double fidelityDecrementScore = root.assets()->json("/universe_server.config:fidelityDecrementScore").toDouble();
    double fidelityIncrementScore = root.assets()->json("/universe_server.config:fidelityIncrementScore").toDouble();

    String serverFidelityMode = root.configuration()->get("serverFidelity").toString();
    Maybe<WorldServerFidelity> lockedFidelity;
    if (!serverFidelityMode.equalsIgnoreCase("automatic"))
      lockedFidelity = WorldServerFidelityNames.getLeft(serverFidelityMode);

    double storageInterval = root.assets()->json("/universe_server.config:worldStorageInterval").toDouble() / 1000.0;
    Timer storageTimer = Timer::withTime(storageInterval);

    TickRateApproacher tickApproacher(1.0 / WorldTimestep, updateMeasureWindow);
    double fidelityScore = 0.0;
    WorldServerFidelity automaticFidelity = WorldServerFidelity::Medium;

    while (!m_stop && !m_errorOccurred) {
      auto fidelity = lockedFidelity.value(automaticFidelity);
      LogMap::set(strf("server_{}_fidelity", m_worldId), WorldServerFidelityNames.getRight(fidelity));
      LogMap::set(strf("server_{}_update_fps", m_worldId), tickApproacher.rate());

      update(fidelity);
      tickApproacher.tick();

      if (storageTimer.timeUp()) {
        sync();
        storageTimer.restart(storageInterval);
      }

      double spareTime = tickApproacher.spareTime();
      fidelityScore += spareTime;

      if (fidelityScore <= fidelityDecrementScore) {
        if (automaticFidelity > WorldServerFidelity::Minimum)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity - 1);
        fidelityScore = 0.0;
      }

      if (fidelityScore >= fidelityIncrementScore) {
        if (automaticFidelity < WorldServerFidelity::High)
          automaticFidelity = (WorldServerFidelity)((int)automaticFidelity + 1);
        fidelityScore = 0.0;
      }

      int64_t spareMilliseconds = floor(spareTime * 1000);
      if (spareMilliseconds > 0)
        Thread::sleepPrecise(spareMilliseconds);
    }
  } catch (std::exception const& e) {
    Logger::error("WorldServerThread exception caught: {}", outputException(e, true));
    m_errorOccurred = true;
  }
}

void WorldServerThread::update(WorldServerFidelity fidelity) {
  RecursiveMutexLocker locker(m_mutex);
  auto unerroredClientIds = m_worldServer->clientIds();
  for (auto clientId : unerroredClientIds) {
    RecursiveMutexLocker queueLocker(m_queueMutex);
    auto incomingPackets = take(m_incomingPacketQueue[clientId]);
    queueLocker.unlock();
    try {
      m_worldServer->handleIncomingPackets(clientId, move(incomingPackets));
    } catch (std::exception const& e) {
      Logger::error("WorldServerThread exception caught handling incoming packets for client {}: {}",
          clientId, outputException(e, true));
      RecursiveMutexLocker queueLocker(m_queueMutex);
      m_outgoingPacketQueue[clientId].appendAll(m_worldServer->removeClient(clientId));
      unerroredClientIds.remove(clientId);
    }
  }

  m_worldServer->setFidelity(fidelity);
  if (!m_pause || *m_pause == false)
    m_worldServer->update();

  for (auto& clientId : unerroredClientIds) {
    auto outgoingPackets = m_worldServer->getOutgoingPackets(clientId);
    RecursiveMutexLocker queueLocker(m_queueMutex);
    m_outgoingPacketQueue[clientId].appendAll(move(outgoingPackets));
  }

  if (m_updateAction)
    m_updateAction(this, m_worldServer.get());
}

void WorldServerThread::sync() {
  RecursiveMutexLocker locker(m_mutex);
  Logger::debug("WorldServer: periodic sync to disk of world {}", m_worldId);
  m_worldServer->sync();
}

}
