# Internals

Why the parts work the way they do. The [README](../README.md) covers using it; this covers the
decisions, mostly so the next person does not have to rediscover them.

## Where the numbers come from

Three sources, in order of preference.

**The status line** is the good one. Claude Code runs a command on every update and hands it a
JSON blob on stdin: context window with its size, both rate-limit windows with reset times,
model, effort, cost, prompt-cache stats. Two lines added to an existing status line script
mirror that payload to a file per session.

**Hooks** carry the attention state, which is nowhere else. `Stop` fires when a turn finishes.
`Notification` with a `permission_prompt` or `elicitation_dialog` type fires when a dialog has
been waiting. `PreToolUse` matched on `AskUserQuestion` catches Claude asking you something.
Each writes a small file, along with the session's `cwd` and transcript path.

**The transcript**, for sessions that run no status line. Claude Code in the desktop app fires
hooks but draws its own usage panel instead of running a status line command, so no payload ever
appears for it. Every assistant record in a transcript carries a `usage` block, so context
tokens, model and version come from a tail read, and they match the status line to the token.

## Context window sizes

The one thing a transcript never states. It cannot be read off the model id either:
`claude-opus-5[1m]` carries a marker only because Opus also runs at 200K, while
`claude-fable-5-1` carries none and is 1M regardless. Guessing 1M for a 200K session understates
it five times over, and understating is the wrong direction to be wrong in for a gauge whose job
is to warn you.

So sizes are **learned** from status line payloads, matched on the model's display name, and
kept on the board. A small seed covers what has actually been observed (an id ending `[1m]`,
Fable at 1M, Haiku 4.5 at 200K) so a fresh board is useful immediately; anything learned
overrides it. A model nobody has been able to measure shows its token count instead of a guessed
percentage.

The two surfaces do not always name a model the same way — the app calls "Opus 5" what the
terminal calls "Opus 5 (1M context)" — so occasionally one has to be taught directly:
`{"win":{"Opus 5":1000000}}` on the serial line or in a `POST /status` body. A mapping the
evidence contradicts is thrown away rather than believed: a session holding more context than
its supposed window has proved that is not its window.

## What the board keeps

The board is the only always-on part of this and the only one every machine talks to, so what
the display learns lives there rather than in a file on whichever laptop saw it first:

- **Model window sizes**, so a laptop that has never run a model in a terminal still gets a real
  percentage as soon as it connects, and a fresh machine inherits the lot by plugging in.
- **The weekly curve.** A payload says how much of the week is spent and how long is left, which
  places a reading on the week's axis without needing a clock on the device. The curve survives
  the laptop sleeping, being closed, or being a different laptop.

Merge on the board, never replace. A host that has just started knows nothing yet, and its first
payload would otherwise erase what every other machine taught it.

## Several machines

Every payload says which machine sent it, and the board keeps a slot per machine rather than
letting the newest overwrite everything. What you see is the merge:

- The **headline** goes to whichever machine is most urgent, on the same
  `needs_input > working > done` order one machine uses across its own windows.
- The **numbers beside it** belong to that same machine, rather than being stitched from two.
- **Counts and session rows** are the sum of all of them, and each row leads with its machine's
  initial once more than one is live.
- A machine not heard from for two minutes has gone to sleep and drops out.

**Rate limits are the exception** and are shared, because they belong to the account rather than
any machine. The board keeps the last reading it saw from anyone, so a machine with none of its
own borrows it whether or not the machine that reported it is still awake. That matters more
than it sounds: the machine *with* a reading is the one with a terminal window open, and the
machine without is one running only the desktop app, which mirrors no status line at all. While
the borrow could only come from a live slot, 5HR and WEEK went blank the moment the laptop
running the terminal slept — or simply had nothing to say for two minutes. A borrowed reading
carries its age, and one older than ten minutes is drawn dim: visible, and visibly not this
minute's. The reset times stay exact however old it is, since they are absolute; the minutes
remaining are counted down from when it was taken.

**The weekly curve is the board's too**, and survives the headline moving between machines. It
used to travel inside the payload struct that a merge replaces wholesale, so it blinked out
whenever the machine that owned the headline was one with no rate limits to draw it from.

Set `CLAUDE_STATUS_NAME` to have a machine report as something other than its hostname.

## Enrolling a machine, and why there is a code

The board serves the installer and the three host files it needs, so enrolling needs `curl` and
a shell and nothing else — no git, no GitHub, no checkout.

The token is the only thing stopping anyone on your network writing to the display, so an
endpoint that handed it to whoever asked would be worse than the inconvenience it saves.
`/install.sh` therefore carries no secret and needs no gate. The token lives behind `/t/<code>`,
and the code is six digits shown only on the board's own screen, so getting one means having
stood in front of the device. Ten minutes after power-up that route stops answering, and a wrong
code gets a 403.

The code is kept in the board's flash rather than minted at boot. Opening the serial port resets
this board, so a fresh code on every boot went stale between reading it off the screen and
typing it.

A few smaller decisions in that path:

- **`-f` is not decoration.** The command ends in `| sh`, so a refusal returned as a 200 with an
  explanation in the body would be piped into a shell. Every refusal is an HTTP error and `-f`
  turns those into a non-zero exit with no output.
- **The prompt works inside the pipeline** because the script reads from `/dev/tty`, which
  `| sh` leaves free. Testing `/dev/tty` with `-r` is not enough: the device node exists whether
  or not the process has a controlling terminal, so the test passes and the open then fails.
- **It is `http://` and cannot reasonably be `https://`.** The board talks TLS as a *client* to
  reach the Claude status page, and that handshake alone wants a 16 KB stack. Serving TLS needs
  a certificate in flash, and no authority issues one for a private address. The only option is
  self-signed, which `curl` rejects without `-k`, and `-k` discards exactly the protection the
  scheme was for.

## Colors

The band color and the LED color both come from `stateRGB()`, so they are the same color by
construction rather than two lists someone has to keep in sync. That was written as an intention
long before it was true: only BUSY derived its band from `stateRGB()`, and every other state
kept a matching constant beside it, which is the arrangement the sentence claims to avoid. Every
colored state derives from it now.

**Warm means the machine wants you.** Your turn and blocked on you were previously amber and
orange — 36 apart on a single channel, on the two states you read most often and must answer
differently, at the low brightness those states run. The color carried nothing and the motion
was doing all the work. Yellow and red-orange now, far enough apart to read across a room.

Two things worth protecting when adding a state: **no green anywhere**, which is what makes the
palette survive red-green color blindness, and magenta's blue content, which is the only thing
keeping RATE LIMIT out of the warm family.
