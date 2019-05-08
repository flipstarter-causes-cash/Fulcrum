#include "Controller.h"
#include "SrvMgr.h"
#include "EXMgr.h"
#include "TcpServer.h"

ClientStateException::~ClientStateException() {} // for vtable

Controller::Controller(SrvMgr *srv, EXMgr *ex)
    : Mgr(nullptr/* top-level because thread*/), srvMgr(srv), exMgr(ex)
{
    setObjectName("Controller");
    _thread.setObjectName(objectName());
}

Controller::~Controller()
{
    cleanup();
}

void Controller::cleanup()
{
    stop(); // no-op if not running
}

void Controller::startup()
{
    start(); // start thread
    // these conns get auto-cleaned on stop() call
    conns.push_back(connect(srvMgr, SIGNAL(newTcpServer(TcpServer *)), this, SLOT(onNewTcpServer(TcpServer *))));
    conns.push_back(connect(exMgr, SIGNAL(gotListUnspentResults(const AddressUnspentEntry &)), this, SLOT(onListUnspentResults(const AddressUnspentEntry &))));
    conns.push_back(connect(exMgr, SIGNAL(gotNewBlockHeight(int)), this, SLOT(onNewBlockHeight(int))));
    conns.push_back(connect(this, SIGNAL(clientDoubleSpendDetected(qint64)), this, SLOT(onClientDoubleSpendDetected(qint64)), Qt::QueuedConnection));
    if (chan.get<QString>(10000).isEmpty())
        throw Exception("Controller startup timed out after 10 seconds");
}

QObject *Controller::qobj() { return this; }

void Controller::on_started()
{
    ThreadObjectMixin::on_started();
    Log() << objectName() << " started";
    chan.put("ok");
}

void Controller::on_finished()
{
    ThreadObjectMixin::on_finished();
    Debug() << objectName() << " finished.";
}

void Controller::onNewTcpServer(TcpServer *srv)
{
    // NB: assumption is TcpServer lives for lifetime of this object. If this invariant changes,
    // please update this code.
    Debug() << __FUNCTION__ << ": " << srv->objectName();

    // attach signals to our slots so we get informed of relevant state changes
    conns.push_back(connect(srv, &TcpServer::newShuffleSpec, this, &Controller::onNewShuffleSpec));
    conns.push_back(connect(srv, &TcpServer::clientDisconnected, this, &Controller::onClientDisconnected));
    const QString srvName(srv->objectName());
    conns.push_back(connect(srv, &QObject::destroyed, this, [srvName]{
        /// defensive programming reminder if invariant is violated by future code changes.
        /// this won't normally ever be reached, as assumption is the controller always dies first.
        Error() << "TcpServer \"" << srvName << "\" destroyed while Controller still alive! FIXME!";
    }));
}

#define server_boilerplate \
    TcpServer *server = dynamic_cast<TcpServer *>(sender()); \
    if (!server) { \
        Error() << __FUNCTION__ << " sender() is either NULL or not a TcpServer! FIXME!"; \
        return; \
    }
void Controller::onClientDisconnected(qint64 clientId)
{
    server_boilerplate
    Debug() << __FUNCTION__ << ": clientid: " << clientId << ", server: " << server->objectName();

    int flushCt = 0;
    if (auto mapServer = clientStates.take(clientId).server /*client is dead, remove entry from map*/;
            mapServer && mapServer != server) {
        // mapServer may be null and that's ok, if client never sent a spec.
        // more defensive programming
        Warning() << __FUNCTION__ << " clientId: " << clientId << " had server: " << mapServer->objectName()
                  << " in map, but sender was: " << server->objectName();
    } else if (mapServer)
        ++flushCt;
    // remove client from cache, if any
    flushCt += removeClientFromAllUnspentCache(clientId);
    if (flushCt)
        Debug("Record of client %lld removed from %d internal data structures", clientId, flushCt);
}
int Controller::removeClientFromAllUnspentCache(qint64 clientId)
{
    int ct = 0;
    // remove client from cache, if any
    for (auto it = addressUnspentCache.begin(); it != addressUnspentCache.end(); ++it) {
        if (it.value().clientSet.remove(clientId)) {
            ++ct;
            it.value().cacheHit(); // mark this as a "cache hit" on unref to keep this cache entry around for a while even if clientset is empty. see onNewBlockHeight()
        }
    }
    return ct;
}
// call this once client's utxo's are all fully verified...
void Controller::refClientToAllUnspentCache(const ShuffleSpec &spec)
{
    if (!spec.isValid()) {
        Error() << __FUNCTION__ << " invalid spec! FIXME! Spec: " << spec.toDebugString();
        return;
    }
    for (auto it = spec.addrUtxo.begin(); it != spec.addrUtxo.end(); ++it) {
        const auto & addr = it.key(); const auto & utxos = it.value();
        if (auto it2 = addressUnspentCache.find(addr); it2 != addressUnspentCache.end()) {
            auto & entry = it2.value();
            if (entry.clientSet.contains(spec.clientId))
                continue;
            for (auto it3 = utxos.begin(); it3 != utxos.end(); ++it3) {
                if (entry.utxoAmounts.contains(*it3)) {
                    entry.clientSet.insert(spec.clientId);
                    break; // client has a utxo in this set, add ref and break out of inner loop
                }
            }
        }
        // proceed to next addr in client spec...
    }
}
void Controller::onNewShuffleSpec(const ShuffleSpec &spec_in)
{
    server_boilerplate
    Debug() << __FUNCTION__ << "; got spec: " << spec_in.toDebugString() << ", server: " << server->objectName();
    ClientState *state = nullptr;
    try {
        if (spec_in.clientId < 0) throw ClientStateException("Spec has invalid clientId!");
        state = &(clientStates[spec_in.clientId]); // create new on not exist, or return existing ref.
        state->gotNewSpec(server, spec_in); // saves server pointer as well as other info from spec, throws on error
    } catch (const std::exception &e) {
        Error() << __FUNCTION__ << ": " << e.what();
        state = nullptr;
        clientStates.remove(spec_in.clientId); // state is now inconsistent. remove from map.
        return;
    }

    ShuffleSpec & spec = state->spec;
    Q_ASSERT(spec.clientId == state->clientId);
    // here we will need to run through spec, check cache, update from listunspent in EX for any unknown UTXOs, etc...
    removeClientFromAllUnspentCache(state->clientId); // since new spec, remove from clientId from all cache entries
    try {
        QSet<BTC::Address> addrsNeedLookup ( analyzeShuffleSpec(spec) );
        if (addrsNeedLookup.isEmpty()) {
            // if we get here it means they all checked out -- tell client everything was accepted
            state->specState = ClientState::Accepted;
            refClientToAllUnspentCache(state->spec);
            emit state->server->tellClientSpecAccepted(spec.clientId, spec.refId);
        } else {
            // no outright rejections, but we still need to do some lookups to verify the utxos they claim exist.
            state->specState = ClientState::InProcess;
            emit state->server->tellClientSpecPending(spec.clientId, spec.refId);
            lookupAddresses(addrsNeedLookup);
        }
    } catch (const std::exception &e) {
        Debug() << "Client spec verification failed: " << e.what();
        state->specState = ClientState::Rejected;
        emit state->server->tellClientSpecRejected(spec.clientId, spec.refId, e.what());
    }
}
#undef server_boilerplate
QSet<BTC::Address> Controller::analyzeShuffleSpec(const ShuffleSpec &spec) const
{
    QSet<BTC::Address> addrsNeedLookup;
    for (auto it = spec.addrUtxo.begin(); it != spec.addrUtxo.end(); ++it) {
        const auto & addr = it.key(); const auto & utxos = it.value();
        if (const auto it2 = addressUnspentCache.find(addr); it2 != addressUnspentCache.end()) {
            // address does exist in cache, check each utxo from client spec
            const auto & entry = it2.value();
            const bool isEntryCurrent = entry.heightVerified >= exMgr->latestHeight(),
                       // >1 min old cache entries are considered too old. expire.
                       // TODO: make this tuneable, also make the cache entries auto-clean
                       // and/or auto-refresh if they have clients subscribed to them
                       isEntryOld = entry.age() >= 60000;
            if (isEntryOld && !isEntryCurrent) {
                addrsNeedLookup.insert(addr);
                continue;
            }
            for (const auto & utxo : utxos) {
                if (!entry.utxoAmounts.contains(utxo)) {
                    if (entry.utxoUnconfAmounts.contains(utxo) && isEntryCurrent) {
                        entry.cacheHit();
                        // they specified an unconfirmed utxo
                        // TODO HERE: break out everything, report error to clientId
                        throw Exception(QString("Unconfirmed UTXO %1; please try again later or with a different utxo").arg(utxo.toString()));
                    } else if (isEntryCurrent) {
                        // utxo is unknown and yet we have a fresh cache entry..
                        // also break out everything here, report error to clientId
                        throw Exception(QString("Unknown UTXO %1 (for address %2); please try again later or with a different utxo").arg(utxo.toString()).arg(addr.toString()));
                    }
                    // else... require refresh
                    addrsNeedLookup.insert(addr);
                    break; // no need to continue scanning all utxos in spec, address itself needs a refresh
                } else {
                    // else.. was in cache, proceed
                    entry.cacheHit(); // mark the cache hit.
                }
            }
        } else {
            // address does not exist in cache, add to lookup set
            addrsNeedLookup.insert(addr);
        }
    }
    return addrsNeedLookup;
}
void Controller::onNewBlockHeight(int height)
{
    // from exMgr
    Debug() << __FUNCTION__ << "; got new block height: " << height;
    static constexpr int COOLDOWN = 15000; // 15 seconds

    /// we wait 15 seconds to do this to give more servers a chance to catch up to the new height
    /// as well as avoid the situation where multiple blocks come in 1 after another.  note the force=true
    /// arg which restarts the timer each time.  This is ok here since this signal arrives only once
    /// per new block height detected.
    callOnTimerSoonNoRepeat(COOLDOWN, "onNewBlockHeight", [this]{
        Debug() << "onNewBlockHeight: new height callback fired... height: " << this->exMgr->latestHeight();
        QSet<BTC::Address> addrsToRemove, addrsToRefresh;
        // loop through entire cache, either marking entries as "for removal" or queueing up a refresh.
        for (auto it = addressUnspentCache.begin(); it != addressUnspentCache.end(); ++it) {
            const auto & addr = it.key();  const auto & entry = it.value();
            // NB: isHeightCurrent has fuzzy race conditions here -- exMgr->latestHeight(), while thread-safe
            // can still get updated at any time. But that's ok.  This is just a cache cleanup & refresh
            // routine and it's ok if it sometimes decides incorrectly (as it's only a cache and not app-logic here).
            const bool isHeightCurrent = entry.heightVerified >= this->exMgr->latestHeight();
            if ( !isHeightCurrent
                    && entry.clientSet.isEmpty()
                    && entry.elapsedLastCacheHit() > COOLDOWN)
            {
                // height is old, has no ref'ing clients, and it hasn't had a cache hit in COOLDOWN msec,
                // so flag for removal
                addrsToRemove.insert(addr);
                continue;
            }
            /// at this point it's either got refing clients or is a reasonably new entry that had a cache hit recently
            /// even if clientset is empty.. so check it if needs update. Usually this branch always is true.
            if ( !isHeightCurrent )
                // verified height is behind, flag for refresh
                addrsToRefresh.insert(addr);
        }
        if (!addrsToRemove.isEmpty()) {
            Debug() << "onNewBlockHeight: removing " << addrsToRemove.size() << " defunct address-unspent cache entries";
            std::for_each(addrsToRemove.begin(), addrsToRemove.end(), [this](const BTC::Address &addr){
                addressUnspentCache.remove(addr);
            });
        }
        if (!addrsToRefresh.isEmpty()) {
            Debug() << "onNewBlockHeight: refreshing " << addrsToRefresh.size() << " still-relevant address-unspent cache entries";
            lookupAddresses(addrsToRefresh);
        }
    }, true);
}
void Controller::onListUnspentResults(const AddressUnspentEntry &entry)
{
    // from exMgr
    Debug() << __FUNCTION__ <<"; got list unspent results: " << entry.toDebugString();
    AddressUnspentEntry *entryPtr = nullptr; ///< this will point to entry in map after below block executes
    bool wasRefresh = false;
    {   // first, update cache, preserving old client id set refs
        const auto oldEntryCopy ( addressUnspentCache.take(entry.address) );
        const auto & oldSetIfAnyCopy ( oldEntryCopy.clientSet );
        const int oldSetSize = oldSetIfAnyCopy.size();
        wasRefresh = oldSetSize;
        if (oldEntryCopy.heightVerified > entry.heightVerified) { // <-- unlikely branch
            // this is an old result that came in later than our current one. put back.
            addressUnspentCache[entry.address] = oldEntryCopy;
            Debug() << "Stale/old listunspent result came in for address \"" << entry.address.toString() << "\""
                    << "; our cached height: " << oldEntryCopy.heightVerified
                    << ", incoming entry height: " << entry.heightVerified << "; ignoring this result!";
            return;
        }
        auto & newEntry = ( addressUnspentCache[entry.address] = entry /* <-- copy it in */ );
        entryPtr = &newEntry;
        newEntry.tsLastCacheHit = oldEntryCopy.tsLastCacheHit; // restore old stats, if any
        newEntry.cacheHitCt = oldEntryCopy.cacheHitCt; // restore old stats, if any
        const int newSetSize = ( newEntry.clientSet += oldSetIfAnyCopy /* restore old client set */).size();
        if (wasRefresh)
            Debug() << "Replaced/freshened existing unspent cache entry for address \"" << entry.address.toString() << "\", total refCt now: " << newSetSize;
    }
    Q_ASSERT(entryPtr);
    // next, search for clients still "in process" and check if they are now 100% verified
    for (auto it = clientStates.begin(); it != clientStates.end(); ++it) {
        ClientState & state = it.value();
        const auto & entry = *entryPtr; // shadows in-param of same name, but this one points to the actual entry in our cache map.
        const bool clientLevelRefreshForAcceptedClient = wasRefresh && state.specState == ClientState::Accepted && entry.clientSet.contains(state.clientId);
        if (state.specState != ClientState::InProcess && !clientLevelRefreshForAcceptedClient)
            // client not in "InProcess" state and/or not a clientLevelRefresh for old cache entry
            continue;
        if (!state.spec.addrUtxo.contains(entry.address))
            // address not relevant to this client
            continue;
        // TODO here -- possibly store in the state the set of addresses this client needs to avoid redundancy
        // or inconsistency here? To be investigated later... -Calin
        try {
            auto addrSet = analyzeShuffleSpec(state.spec);
            if (addrSet.isEmpty()) {
                // yay! accepted!
                if (!clientLevelRefreshForAcceptedClient) {
                    // only notify of "we are good now"  if the previous state was not Accepted (eg not a clientLevelRefresh)
                    state.specState = ClientState::Accepted;
                    refClientToAllUnspentCache(state.spec);
                    emit state.server->tellClientSpecAccepted(state.spec.clientId, NO_ID);
                } else {
                    Debug() << "address \"" << entry.address.toString() << "\" refreshed for clientId: " << state.spec.clientId;
                }
            } else if (addrSet.contains(entry.address)) {
                // ruh-roh -- they may have extra UTXOs in their spec that aren't in the unspent list.
                throw Exception(QString("Could not verify UTXOs for address \"%1\"").arg(entry.address.toString()));
            }
        } catch (const std::exception & e) {
            // Note it's possible for a client that was previously in the accepted state to end up here
            // if they spent their UTXOs that were previously accepted.  In this case they will get a 'notification'
            // type message of method "shuffle.spec", with params=["message"] where message will be the error message
            // explaining it's a double-spend.
            // To recap:
            // If it is immediately accepted, the { "result" : "accepted" } will be sent as a RESPONSE message.
            // If it is pending, the { "result" : "pending" } will be the RESPONSE (result) message.
            // Upon acceptance, a notification will be sent with: { "method" : "shuffle.spec", "params" : ["accepted"] }
            // Additionally, on double-spend, a notification may later appear:
            //    { "method" : "shuffle.spec", "params" : ["you double spent error message bla bla bla"] }
            // So lifecycle can be:
            //    "pending" -> "accepted" -> "rejected"
            const bool doubleSpend = state.specState == ClientState::Accepted;
            state.specState = ClientState::Rejected;
            removeClientFromAllUnspentCache(state.clientId);
            emit state.server->tellClientSpecRejected(state.spec.clientId, NO_ID, e.what());
            if (doubleSpend)
                emit clientDoubleSpendDetected(state.clientId);
        }
    }
}

void Controller::lookupAddresses(const QSet<BTC::Address> &addrs)
{
    Debug() << __FUNCTION__ << "; addrs = " << Util::Stringify(addrs);

    if (addrs.isEmpty()) return;
    pendingAddressLookups += addrs;

    // for now we queue these up and do them in batckes on a timer that won't run more often than once every 250ms
    // TODO: tweak this or maybe remove if it's not needed.
    callOnTimerSoonNoRepeat(250, "AddressLookups", [this]() {
        for (const auto & addr : pendingAddressLookups) {
            emit exMgr->listUnspent(addr);
        }
        pendingAddressLookups.clear();
    });
}

void Controller::onClientDoubleSpendDetected(qint64 clientId)
{
    Debug() << __FUNCTION__ << ": clientId: " << clientId;
    // todo: implement some sort of mitigation and/or penalty and/or cancel extant shuffles, etc...
}

Mgr::Stats Controller::stats() const
{
    Stats ret;
    ret["ElectrumX_Mgr"] = exMgr->statsSafe();
    ret["TCPSrv"] = srvMgr->statsSafe();
    return ret;
}

QString ShuffleSpec::toDebugString() const
{
    QString ret;
    {
        QTextStream ts(&ret);

        ts << "(Shuffle Spec; clientId: " << clientId << "; refId: " << refId << "; <amounts: ";
        for (const auto amt : amounts)
            ts << amt << ", ";
        ts << ">; shuffleAddr: " << shuffleAddr.toString() << "; changeAddr: " << changeAddr.toString();
        ts << "; <addrs->utxos: ";
        for (auto it = addrUtxo.begin(); it != addrUtxo.end(); ++it) {
            ts << "[addr: " << it.key().toString() << " utxos: ";
            ts << Util::Stringify(it.value()) << "], ";
        }
        ts << ">)";
    }
    return ret;
}

QString AddressUnspentEntry::toDebugString() const
{
    QString ret;
    {
        QTextStream ts(&ret);
        ts << "(AddressUnspentEntry; address: " << address.toString() << "; heightVerified: " << heightVerified << "; tsVerified: " << tsVerified
           << "; <clients: ";
        for (const auto c : clientSet) {
            ts << c << ", ";
        }
        ts << ">; <UTXO Amounts: ";
        for (auto it = utxoAmounts.begin(); it != utxoAmounts.end(); ++it) {
            ts << it.key().toString() << "=" << it.value() << " sats, ";
        }
        ts << ">; <UTXO Unconf. Amounts: ";
        for (auto it = utxoUnconfAmounts.begin(); it != utxoUnconfAmounts.end(); ++it) {
            ts << it.key().toString() << "=" << it.value() << " sats, ";
        }
        ts << ">)";
    }
    return ret;
}