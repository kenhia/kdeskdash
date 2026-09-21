/**
 * @file panel_store.h
 * The panel's durable state, as the rest of the program sees it: one
 * process-wide store over the state file (panel_state.h), write-through.
 *
 * This is the half of sprint 039 that lets the board stop running a Redis
 * server of its own. The API is deliberately the shape the `redis_*` getters
 * and setters had — `get(default)` returning the caller's default when the
 * value is absent, `incr` returning the post-increment count — so the modes
 * changed a name and nothing else.
 *
 * Every mutation rewrites the file atomically. That is a few hundred bytes on
 * a GoLZ round ending, a calculator store, a dev assignment or a swipe; the
 * alternative (batching, or a flush on shutdown) would lose exactly the state
 * a power cut is most likely to interrupt, which is the state this file exists
 * to keep.
 *
 * A failed write is reported once and then swallowed: the panel keeps running
 * on its in-memory copy, the way it used to keep running with Redis down.
 */
#ifndef KDESKDASH_PANEL_STORE_H
#define KDESKDASH_PANEL_STORE_H

#include <stdbool.h>
#include <stddef.h>

/* Which dev-mode chart side a host assignment belongs to. */
typedef enum {
    PANEL_DEV_SIDE_LEFT = 0,
    PANEL_DEV_SIDE_RIGHT,
} panel_dev_side_t;

/* Load `path` (PANEL_STATE_DEFAULT_PATH when NULL/empty) into the store.
 *
 * When the file is ABSENT — and only then — this performs the ONE-TIME
 * MIGRATION: it connects to the legacy control Redis described by
 * `legacy_host`/`legacy_port`/`legacy_auth` and copies the `kdeskdash:*` values
 * across, then writes the file. It COPIES, never moves: nothing is deleted
 * from Redis, so an older build rolled back onto the same board still finds
 * its state where it left it.
 *
 * A file that exists but was REJECTED does not migrate. By then the Redis
 * values are stale, and quietly resurrecting them would look exactly like the
 * file having been fine.
 *
 * Pass `legacy_host = NULL` to skip the migration entirely (no Redis is
 * dialled). Safe to call when the endpoint is absent or unreachable: the
 * migration is best-effort and its failure is a log line, not a stop.
 */
void panel_store_init(const char *path, const char *legacy_host,
                      int legacy_port, const char *legacy_auth);

/* Post-increment counters for the post-machete GoLZ outcomes. Return the new
 * count, or -1 if it could not be recorded (caller treats < 0 as unknown and
 * falls back to its in-memory mirror, exactly as it did with Redis down). */
long panel_store_golz_incr_human_wins(void);
long panel_store_golz_incr_zombie_wins(void);
long panel_store_golz_incr_ties(void);

/* Read a counter, returning `default_val` when the state file does not carry
 * it. */
long panel_store_golz_get_human_wins(long default_val);
long panel_store_golz_get_zombie_wins(long default_val);
long panel_store_golz_get_ties(long default_val);

/* The legacy pre-machete zombie-win counter. Display-only: nothing increments
 * it any more, and the migration copies it across so the historical figure
 * survives the move off Redis. */
long panel_store_golz_get_wins(long default_val);

/* The adaptive Human-win generation threshold. get returns `default_val` when
 * absent; both enforce the game-rule floor of 100, and set returns the value
 * actually stored (still the floored value when the write fails, so the caller
 * can mirror it in memory). */
long panel_store_golz_get_gens_to_win(long default_val);
long panel_store_golz_set_gens_to_win(long value);

/* The calculator's store/recall registers, as produced by
 * calc_regs_serialize. An empty or NULL line clears the saved registers.
 * The value read back is untrusted — hand it to calc_regs_parse, which rejects
 * a malformed line whole rather than half-restoring the register file. */
void panel_store_set_calc_regs(const char *line);
bool panel_store_get_calc_regs(char *buf, size_t buflen);

/* A dev-mode host assignment. An empty or NULL host clears the slot. The value
 * read back is untrusted — re-validate it against the host-token contract
 * before using it to build a telemetry key. */
void panel_store_set_dev_assignment(panel_dev_side_t side, const char *host);
bool panel_store_get_dev_assignment(panel_dev_side_t side, char *buf,
                                    size_t buflen);

/* The last active mode id. Written on every change (the shell's change
 * callback), read once at startup to restore. */
void panel_store_set_active_mode(const char *id);
bool panel_store_get_active_mode(char *buf, size_t buflen);

#endif /* KDESKDASH_PANEL_STORE_H */
