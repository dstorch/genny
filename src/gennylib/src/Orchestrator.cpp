#include <algorithm>  // std::max
#include <cassert>
#include <iostream>
#include <shared_mutex>

#include <gennylib/Orchestrator.hpp>

namespace {

// so we can call morePhases() in awaitPhaseEnd.
inline constexpr bool morePhaseLogic(int currentPhase, int maxPhase, bool errors) {
    return currentPhase <= maxPhase && !errors;
}

}  // namespace


namespace genny {

/*
 * Multiple readers can access at same time, but only one writer.
 * Use reader when only reading internal state, but use writer whenever changing it.
 */
using reader = std::shared_lock<std::shared_mutex>;
using writer = std::unique_lock<std::shared_mutex>;

unsigned int Orchestrator::currentPhaseNumber() const {
    reader lock{_mutex};

    return this->_phase;
}

bool Orchestrator::morePhases() const {
    reader lock{_mutex};

    return morePhaseLogic(this->_phase, this->_maxPhase, this->_errors);
}

// we start once we have required number of tokens
unsigned int Orchestrator::awaitPhaseStart(bool block, int addTokens) {
    writer lock{_mutex};
    assert(state == State::PhaseEnded);

    _currentTokens += addTokens;

    unsigned int currentPhase = this->_phase;
    if (_currentTokens >= _requireTokens) {
        _phaseChange.notify_all();
        state = State::PhaseStarted;
    } else {
        if (block) {
            while (state != State::PhaseStarted) {
                _phaseChange.wait(lock);
            }
        }
    }
    return currentPhase;
}

void Orchestrator::addRequiredTokens(int tokens) {
    writer lock{_mutex};

    this->_requireTokens += tokens;
}

void Orchestrator::phasesAtLeastTo(unsigned int minPhase) {
    writer lock{_mutex};
    this->_maxPhase = std::max(this->_maxPhase, minPhase);
}

// we end once no more tokens left
bool Orchestrator::awaitPhaseEnd(bool block, int removeTokens) {
    writer lock{_mutex};
    assert(State::PhaseStarted == state);

    _currentTokens -= removeTokens;

    // Not clear if we should allow _currentTokens to drop below zero
    // and if below check should be `if (_currentTokens == 0)`.
    //
    // - defensive programming says that a bug could cause it to go below zero
    //   and if that happens and we're comparing == 0, then the workload will
    //   block forever
    //
    // - an Actor could get clever by wanting the tokens to dip below zero if
    //   it knows it will level-them out or whatever in the future
    //
    // - BUT: there's no real existing good reason why an actor would
    //   want to do this, so it's likely an error. Presumably such errors
    //   will be caught in automated testing, though, so adding a runtime
    //   check seems to limit the functionality unnecessarily.
    //
    // Similar thing applies to the block in awaitPhaseStart() where we
    // compare with >= rather than ==.

    if (_currentTokens <= 0) {
        ++_phase;
        _phaseChange.notify_all();
        state = State::PhaseEnded;
    } else {
        if (block) {
            while (state != State::PhaseEnded) {
                _phaseChange.wait(lock);
            }
        }
    }
    return morePhaseLogic(this->_phase, this->_maxPhase, this->_errors);
}

void Orchestrator::abort() {
    writer lock{_mutex};

    this->_errors = true;
}

}  // namespace genny