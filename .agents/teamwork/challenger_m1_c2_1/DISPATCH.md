# DISPATCH — challenger_m1_c2_1

## Task Assignment
**Role**: Adversarial Challenger (On-Demand Connection & Candidate Endpoints)
**Working Directory**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c2_1`
**Project Plan**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
**Source of Truth**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (read section `## 2026-10-23T12:34:22Z`)
**Worker Handoff**: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c2/handoff.md`

## Challenge Scope
Empirically stress-test the new on-demand connection logic:
1. Write a standalone test harness or unit test exercising `Engine::ensure_connected()` and `Engine::send_frame_active()` when initial `active_channel_` is null.
2. Test scenarios:
   - Candidate list has an unreachable IP: verify it fails within 1000ms timeout and does NOT hang the thread.
   - Candidate list has an unreachable IP followed by a valid listening port: verify fallback succeeds and frame is sent.
   - Concurrency stress: 10 threads concurrently calling `send_clipboard` or `ensure_connected()`: verify no deadlock, no double-connect, and no race conditions.
3. Deliver verdict (APPROVE or REQUEST_CHANGES) with execution logs in `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c2_1/handoff.md`.

## 2026-10-23T13:08:44Z
You are challenger_m1_c2_1.
Working Directory: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c2_1`
Read:
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/ORIGINAL_REQUEST.md` (section `## 2026-10-23T12:34:22Z`)
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/PROJECT.md`
- `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/worker_m1_c2/handoff.md`
- Your dispatch file: `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c2_1/DISPATCH.md`

Objective:
Empirically stress-test `Engine::ensure_connected()` and on-demand connection:
- Unreachable IP timeout (verifying 1000ms poll timeout without thread hang).
- Fallback to reachable endpoint.
- Concurrent multi-threaded connection attempts (no deadlock, no double-connect).
Deliver verdict (APPROVE or REQUEST_CHANGES) in `/home/reid-pu/Programming/Linux_to_Android/.agents/teamwork/challenger_m1_c2_1/handoff.md` and send message when done.
