#include "DeckManager.hpp"

namespace desk32::engine::deck {

DeckManager::DeckManager(double graphRate)
    : m_graphRate(graphRate)
{
    for (size_t i = 0; i < 4; ++i) {
        char id = static_cast<char>('A' + i);
        m_decks[i] = std::make_unique<ReferenceDeck>(id);
    }
}

DeckManager::~DeckManager() = default;

} // namespace desk32::engine::deck
