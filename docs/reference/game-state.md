# Game state

The games' own state: saves, scripted state, and behaviour that looks like a host bug but is not. Each entry: symptom, cause, fix, the games it was found on. The [symptom index](../agents/symptoms.md) lists every symptom.

## Game behaviour that is not a host bug

### Most draws are in command buffers replayed later

- **Symptom:** "stuck" at a screen with one draw per frame.
- **Cause:** usually not stuck: most draws are in recorded command buffers replayed later.
- **Fix:** dump the passes (`DM_NVN_PASSDUMP`, [04, Diagnostic switches](../agents/04-graphics.md#7-diagnostic-switches-host)) before assuming the game logic is blocked.
