#include <cassert>

#include "server/gameplay/mission/enemy_wave_queue.h"

using namespace sunrise::server::gameplay::mission;

int main() {
    reset_enemy_waves();
    EnemyWaveIntent intent{};
    intent.commandId = 1;
    intent.waveId = 2;
    intent.spawnerDefinition = 0x80C26B0A;
    intent.requested = {1, 0};
    intent.requestedCount = 2;
    EnemyWaveTicket first{};
    EnemyWaveTicket duplicate{};
    assert(reserve_enemy_wave(10, 20, 30, intent, first));
    assert(reserve_enemy_wave(10, 20, 30, intent, duplicate));
    assert(first.value != 0 && first.value == duplicate.value);
    QueuedEnemyWave queued{};
    assert(!peek_enemy_wave(10, 21, 30, queued));
    assert(!peek_enemy_wave(10, 20, 31, queued));
    assert(peek_enemy_wave(10, 20, 30, queued) && !queued.published);
    assert(queued.bindingGeneration == 30);
    assert(!settle_enemy_wave(first));
    assert(mark_enemy_wave_published(first));
    assert(peek_enemy_wave(10, 20, 30, queued) && queued.published);
    assert(settle_enemy_wave(first));
    assert(!peek_enemy_wave(10, 20, 30, queued));

    intent.commandId = 2;
    assert(reserve_enemy_wave(10, 20, 30, intent, first));
    cancel_enemy_waves(10, 21, 30);
    cancel_enemy_waves(10, 20, 31);
    assert(peek_enemy_wave(10, 20, 30, queued));
    cancel_enemy_waves(10, 20, 30);
    assert(!peek_enemy_wave(10, 20, 30, queued));
}
