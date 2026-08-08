# zkgram

**A real end-to-end encrypted messenger that runs on top of your existing Telegram account.**

zkgram is a native desktop Telegram client built on [TDLib](https://github.com/tdlib/td), with a genuine, independently implemented end-to-end encryption layer placed in front of every message before it ever touches Telegram's servers. Telegram does not see your conversation. It only ever sees ciphertext, disguised as ordinary text, flowing between two accounts that happen to talk to each other.

The encryption itself is a modified version of [CryptoLayer](https://github.com/igmunv/cryptolayer), extended with Telegram transport support and encrypted file transfer specifically for this project, maintained at [selimann/cryptolayer-modules](https://github.com/selimann/cryptolayer-modules).

You keep your existing Telegram account, your existing contacts, and Telegram's own reliable message delivery. What changes is that, for any conversation where both sides run zkgram, the content itself is invisible to Telegram, to anyone who might compromise Telegram's servers, and to anyone intercepting traffic in between. Everyone else on the network still sees you as an ordinary Telegram user.

## Why this exists

Telegram's own "Secret Chats" are end-to-end encrypted, but they are a separate, second-class feature: no multi-device support, no cloud sync, and you have to trust Telegram's own implementation of the protocol. Regular Telegram chats, the ones people actually use day to day, are encrypted only in transit and at rest on Telegram's servers, not end to end. Telegram itself, and by extension anyone who gains access to Telegram's infrastructure, can in principle read them.

zkgram closes that gap without asking you to leave Telegram, create a new account, or convince your contacts to install a completely separate app with its own network and its own reliability problems. It reuses Telegram purely as a transport, the way a postal service is used to deliver a sealed, tamper-evident envelope: the courier can see that a letter moved from one address to another, but not what is written inside it.

## Core capabilities

### Real end-to-end encryption, not a marketing label

- Every encrypted conversation is its own independent session: a fresh elliptic-curve Diffie-Hellman key exchange (SECP256R1/ECDH) happens each time a session starts, producing a symmetric AES key that only exists on the two participating devices, in memory, for the lifetime of that session.
- Messages are encrypted with AES-GCM before they leave your device, then compressed and wrapped, and only the resulting ciphertext is ever transmitted through Telegram.
- Because the encryption key is negotiated fresh on every new session (forward secrecy), a device compromised after the fact cannot retroactively decrypt what it captured earlier, and closing and reopening a session does not reuse an old key.
- Every participant has a persistent signing identity (a separate ECDSA keypair) used to sign the key exchange itself, so a man-in-the-middle cannot silently swap in their own key during the handshake. The app shows both sides' signature fingerprints so they can be compared and confirmed, exactly the way Signal or WhatsApp safety numbers work.

### Disguised ciphertext, not obvious encrypted blobs

Raw ciphertext looks nothing like text, and a message that is visibly "encrypted garbage" is itself a signal that something suspicious is happening in that conversation. zkgram encodes ciphertext through a word-substitution scheme (WordCoder) before sending it, so what actually travels over Telegram reads as a sequence of ordinary words rather than a block of random-looking bytes. Telegram's own systems, and anyone glancing at the conversation who is not one of the two participants, see plausible-looking text, not an obvious marker that says "this message is encrypted."

### Runs on your real Telegram account, over Telegram's real infrastructure

zkgram authenticates as a real Telegram client (via TDLib, the same library the official desktop and mobile apps are built on), using your phone number and Telegram's own login flow, including two-factor authentication. There is no separate zkgram account, no separate server operated by this project, and no dependency on this project's own infrastructure staying online. Message delivery, retries, offline queuing, and multi-device presence are all inherited from Telegram itself. Encryption is layered on top, chat by chat, entirely under your control.

### One-way secret/normal separation

Not every conversation needs to be encrypted, and not every moment of looking at your screen is a moment you want decrypted content visible. Each chat can be individually switched into an encrypted session, and once it has any encrypted history at all, its view can be independently toggled between:

- **Normal mode**: shows only ordinary Telegram content for that chat. Decrypted content is never drawn, never leaked into this view, regardless of whether an encrypted session exists for that chat.
- **Secret mode**: shows ordinary content plus the decrypted conversation, explicitly revealed.

This is deliberately one-directional: normal mode can never accidentally surface secret content, while secret mode can still show the ordinary parts of the conversation for context. The distinction is enforced at the data level, not just hidden visually, and defaults to off, including for previously cached encrypted history from an earlier session.

### Local encrypted history that survives a restart

Forward secrecy means the encryption key for a session is never reused and never written to disk. That is the correct security property, but taken alone it also means that without any further work, closing the app would make an entire conversation's history permanently unreadable the moment the process exits, even on the very same device that just received it. zkgram solves this the same way real Telegram Desktop solves the equivalent problem for its own local data: a local cache, encrypted at rest with a key derived from your own local password (PBKDF2-HMAC-SHA256, AES-256-CTR), stored next to your other application data, never anything Telegram itself ever sees.

### A genuine multi-conversation client

zkgram is not a single-purpose "type a message to one hardcoded contact" tool. It loads your real Telegram chat list, and any contact you choose can independently have an encrypted session started with them, run alongside as many other encrypted (or plain) conversations as you like, each with its own handshake, its own keys, and its own local history.

### Encrypted files and voice messages

Files sent through an active encrypted session are encrypted and authenticated (AES-GCM, signed with the sender's own signing key) before upload, and verified and decrypted on arrival. Voice messages are recorded locally and sent through the exact same encrypted path as any other file.

### Interface that matches the real Telegram Desktop client

Rather than approximating what Telegram Desktop looks like, zkgram's interface is built directly against the real client's own source and design assets: real icon artwork extracted from Telegram Desktop's own resources, the same composer layout and proportions, the same kinetic scrolling physics tuning, and the same color palette pulled from Telegram Desktop's own theme files. Message history is virtualized (only what is actually visible is ever drawn or laid out), supports real click-and-drag text selection, and only the message content itself is selectable, never timestamps or delivery indicators.

## How it works

zkgram is organized into three layers that are deliberately kept from knowing about each other directly:

1. **Transport** (`telegram::TelegramClient`): talks to TDLib, handles login, the chat list, sending and receiving raw bytes and files. Has no idea what it is carrying is encrypted.
2. **Cryptography** (`crypto::CryptoLayer`): an embedded Python bridge into the real [CryptoLayer](https://github.com/igmunv/cryptolayer) engine, which performs the actual key exchange, signing, encryption, and decryption. Has no idea Telegram exists; it only knows it has bytes to send and bytes it received.
3. **Session** (`core::Session`): the only layer allowed to know about both of the above. It wires a specific chat's transport events to a specific encrypted conversation's cryptography, and nothing else in the codebase is allowed to reach across that boundary.

A pluggable UI layer sits on top of `core::Session` through a small interface (`UiProvider`), so the interface itself, currently a Qt desktop client, is not hardwired into the rest of the application and could in principle be replaced or extended.

Full architecture documentation, including the exact data flow, the Python bridge's threading model, and how to extend the project with a new cryptographic backend, a different transport, or a different UI, is in [`docs/README.md`](docs/README.md).

## What zkgram does not protect against

Being precise about limits is part of taking security seriously.

- **Metadata is not hidden.** Telegram still knows which two accounts are communicating and roughly when, the same way a postal service knows which two addresses exchanged a letter even without opening it. zkgram protects the content of a conversation, not the fact that a conversation is happening.
- **A compromised device defeats everything running on it.** If your device itself is compromised while a session is active, or while you are looking at decrypted content on screen, no amount of encryption in transit helps. This is true of every messenger, not specific to zkgram.
- **Trust in the signature exchange is still on you.** The app shows you both sides' signature fingerprints to compare; skipping that comparison (or comparing carelessly) weakens the guarantee that a handshake is with the person you think it is with.
- **The local password is not recoverable.** It decrypts your local identity file and is never stored anywhere, by design. Losing it means generating a new local identity.

## Project status

- Login, the full chat list, sending and receiving both plain and encrypted messages, multiple simultaneous encrypted conversations, file and voice message transfer, local encrypted history caching, and the secret/normal mode separation are implemented and have been exercised against a real Telegram account.
- Windows is the primary, most thoroughly tested target. Linux is supported by the build (`build.sh`, `installtdlib.sh`) but has seen less real-world testing. macOS should follow the same build path as Linux but has not been verified.
- Not yet implemented: a proper installer (currently a portable build only), group encryption for more than two participants, and a small number of composer icons that still use hand-drawn approximations rather than the real Telegram Desktop asset. See [`TODO.md`](TODO.md) for the current, actively maintained list (kept locally, not part of the published repository).

## Getting started

On Windows, one command installs every prerequisite (TDLib, Qt) and produces a ready-to-run portable build in `dist/`:

```powershell
.\build.ps1
```

Re-running it is safe and incremental: TDLib and Qt are only installed once and reused, so later runs just rebuild zkgram itself. `-Clean` forces a full rebuild from scratch, `-SkipTdlib`/`-SkipQt` skip a step you already have installed, `-OutDir` changes where the portable build is placed.

On Linux, use `build.sh`/`installtdlib.sh` in place of the PowerShell scripts above.

Manual, step-by-step build instructions without the helper script are documented in [`docs/README.md`](docs/README.md).

### Requirements

Required for any build:

- CMake 3.20 or newer
- A C++20 compiler (MSVC 19.3x+ on Windows via Visual Studio 2022 Build Tools, GCC/Clang on Linux)
- Python 3.8 or newer with development headers, plus `pip install cryptography brotli` in that same interpreter (this is what the embedded CryptoLayer engine actually runs on)
- [TDLib](https://github.com/tdlib/td), built from source (no precompiled binaries are published or fetched, deliberately: this is a cryptographic component, and trusting someone else's opaque binary of it defeats the point). `install-tdlib.ps1`/`installtdlib.sh` automate this.
- [OpenSSL](https://openssl.org/)

Required only for the graphical client (`-DZKGRAM_UI=qt`, the default; a console-only build needs none of this):

- Qt6, `Widgets` and `Multimedia` modules, built with a compiler ABI-compatible with the rest of the toolchain (an MSVC build of Qt on Windows, not a MinGW one)

Full dependency details, manual build instructions without the helper scripts, and the `-DZKGRAM_UI` option are documented in [`docs/README.md`](docs/README.md).

Run `zkgram.exe`/`zkgram` from the repository root, or from the portable build's own folder (`dist/`), so the embedded Python bridge can find CryptoLayer next to it.

## License

zkgram is licensed under the [GNU GPL version 3](https://www.gnu.org/licenses/gpl-3.0.html). Part of the interface code (`src/ui/qt/`) reuses design and structure from [tdesktop](https://github.com/telegramdesktop/tdesktop), the official Telegram Desktop client, itself GPLv3, which is what requires this licensing. The full license text is in [`LICENSE`](LICENSE).

## Acknowledgments

- [TDLib](https://github.com/tdlib/td), the official Telegram client library this project's transport layer is built on.
- [CryptoLayer](https://github.com/igmunv/cryptolayer), the original encryption engine this project builds on; zkgram uses a modified version with added Telegram transport and encrypted file transfer support, maintained at [selimann/cryptolayer-modules](https://github.com/selimann/cryptolayer-modules).
- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop), whose real source and design assets zkgram's interface is deliberately built to match rather than approximate.
