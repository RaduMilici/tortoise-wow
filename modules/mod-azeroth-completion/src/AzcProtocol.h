#ifndef AZC_PROTOCOL_H
#define AZC_PROTOCOL_H

#include "AzcProgress.h"

class Player;

namespace Azc
{
    // Builds the record payload of the addon protocol (see PROTOCOL.md):
    //   TYPE^key=value^key=value;TYPE^...
    class RecordWriter
    {
    public:
        RecordWriter& Rec(char const* type);
        RecordWriter& Kv(char const* key, std::string const& value);
        RecordWriter& Kv(char const* key, char const* value) { return Kv(key, std::string(value ? value : "")); }
        RecordWriter& Kv(char const* key, uint32 value) { return Kv(key, std::to_string(value)); }
        RecordWriter& Kv(char const* key, int32 value) { return Kv(key, std::to_string(value)); }
        RecordWriter& Kv(char const* key, uint64 value) { return Kv(key, std::to_string(value)); }
        RecordWriter& Kv(char const* key, bool value) { return Kv(key, value ? "1" : "0"); }
        RecordWriter& Kv(char const* key, float value);
        RecordWriter& KvList(char const* key, std::vector<uint32> const& values);
        RecordWriter& KvIf(char const* key, std::string const& value) { return value.empty() ? *this : Kv(key, value); }

        std::string const& Str() const { return m_out; }
        bool Empty() const { return m_out.empty(); }
        size_t Size() const { return m_out.size(); }

    private:
        std::string m_out;
    };

    std::string Escape(std::string const& text);

    // Consumes "AZC\t..." addon messages. Returns false for anything else.
    bool HandleAddonMessage(Player* player, std::string const& msg);

    // Sends events to the addon (when active) and/or as chat lines. Store lock must be held.
    void DeliverEvents(Player* player, PlayerState& state, std::vector<Event> const& events);

    // Shared by the addon protocol and the chat commands.
    struct Suggestion
    {
        std::string kind;       // explore, quest, rare, elite, travel
        std::string id;         // objective id
        std::string text;
        uint32 questId = 0;
        bool hasLocation = false;
        Point location;
        float distance = -1.0f;
    };
    std::vector<Suggestion> BuildSuggestions(Player* player, PlayerState const& state, Definitions const& defs, ZoneEval const& eval);

    std::string QuestLine(QuestEval const& q);      // "The Defias Brotherhood V - starts at Gryan Stoutmantle (Sentinel Hill)"
}

#endif
