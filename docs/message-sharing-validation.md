# Universal message sharing

The common entry is `AddMessageShareAction` / `PrepareMessageShare` in
`Telegram/SourceFiles/api/api_message_share.cpp`. `FastShareMessage` remains an
album-aware single-message wrapper. The common predicate excludes service
messages, calls, gifts and wallpaper records; it does not inspect source chat
permissions, publicity, message forwarding rights or media presence.

## Screen matrix

Each adapter passes a single item, the clicked album, or the exact selected
items to the common entry. Selection is ordered and deduplicated before the
widget opens. Source adapters were inspected and compiled in Release. The real
history context menu and recipient widget were also exercised visually using
synthetic data.

| Surface | Adapter | Selection / special handling |
| --- | --- | --- |
| Main history, groups, bots, private/public channels, Saved Messages | `history/history_inner_widget.cpp` | Exact selection; album versus individual element |
| Modern history, forum topics, comments, community chats, channel correspondence | `history/view/history_view_context_menu.cpp` | Common `AddMessageActions`; exact selection |
| Saved subsections and poll lists | `history/view/history_view_context_menu.cpp` | Shared `ListWidget` adapter |
| Pinned messages | `history/view/history_view_context_menu.cpp` | Shared `ListWidget` adapter |
| Scheduled messages | `history/view/history_view_context_menu.cpp` | Local content snapshots |
| Business shortcuts and welcome messages | `history/view/history_view_context_menu.cpp` | Local content snapshots |
| Search results | `dialogs/dialogs_inner_widget.cpp` | Result message, independently of chat entry actions |
| Chat preview | `history/view/history_view_list_widget.cpp` | Share-only menu when no session controller is attached to the list |
| Shared media, global media, downloads | `info/media/info_media_list_widget.cpp` | Exact global message IDs; no forwarding restriction on selection |
| Administrator log | `history/admin_log/history_admin_log_inner.cpp` | Snapshot of displayed content; service entries excluded |
| Media viewer | `media/view/media_view_overlay_widget.cpp` | Current element; snapshot before closing viewer |
| Article / rich message viewer | `iv/iv_instance.cpp` | Rich source uses common message sharing; ordinary web article retains link sharing |
| Single-view media layer | `chat_helpers/ttl_media_layer_widget.cpp` | Context action with a snapshot callback |

No persistent arrows were added. Existing native Forward actions remain
independent of Share. Recipient permissions are checked against the prepared
content.

## Content matrix

The current media classes are enumerated in `data/data_media_types.h`.
Automatic mode uses native forwarding when both the source and recipient permit
it. The table describes copying and fallback.

| Content / model | Prepared copy |
| --- | --- |
| Text, bot text, channel text | Complete `TextWithEntities`, split only at message length limits |
| Link preview / `MediaWebPage` | Standalone text; preview photo/document is not treated as an attachment |
| `MediaPhoto` | Existing photo reference and formatted caption |
| `MediaFile`: document, video, audio, voice, round video, GIF, sticker | Existing document reference, preserving document type |
| `MediaContact` | Phone, names and original vCard |
| `MediaLocation` | Geo point or venue with title/address |
| `MediaPoll`, quiz | Existing full clipboard representation with question/answers and available link |
| `MediaTodoList` | Full title/items and available link |
| `MediaGame` | Title, description, consumed message text and available link |
| `MediaInvoice`, including paid content containers | Title, description and available source link |
| `MediaDice` | Existing textual representation |
| `MediaStory` inside a message | Existing text and available source link |
| `MediaGiveawayStart`, `MediaGiveawayResults` | Existing textual representation and available source link |
| Unknown / unsupported media | Available full message text or existing representation; empty content is rejected |
| Rich message | Full text, table cells, nested blocks, button URLs, embedded HTML text, media and maps in order |
| Service actions; `MediaCall`, `MediaGiftBox`, `MediaWallPaper` | No Share action |

An oversized media caption is emitted in full after the selected album. An
album requiring both native and copy delivery is copied as a group. “Hide
Captions” preserves standalone text and the comment. Rich fallback never uses
the notification summary or synthesizes an empty-message label.

## Executed checks

```sh
cmake --build out --config Release --target Telegram test_message_share -j 6
out/Telegram/Release/test_message_share
git diff --check
```

The Release build and tests pass. The final incremental build reported no
remaining work. No Debug configuration was used.

The permanent `tests/test_message_share.cpp` checks:

- Native/copy mixtures, exact selected album members, multiple source chats/accounts,
  grouping boundaries and API batch limits.
- Bold, links, spoilers and custom emoji; comments; caption removal in both
  copying and mixed automatic delivery; long captions; unavailable content.
- Actual serialized requests for text, media, albums and native forwarding:
  destination, forum topic, monoforum recipient, silent mode, schedule,
  payment budget, formatting, vCard and stable random IDs.
- Absence of `clear_draft` flags.
- A substituted transport running the production delivery queue: repeated
  clicks, ACK-only advancement, rejection, partial completion, retry of only
  the rejected part, and blocked retry when delivery is uncertain.

A disposable Release overlay also exercised the real client model and widgets
with synthetic accounts. Its application bundle and profile were separate
from the installed app. IP networking was denied for the successful runs.
The overlay was not added to production sources.

The model matrix passed 210 combinations: 21 content shapes across Saved
Messages, a user, a basic group, a bot, a broadcast, a protected/read-only
broadcast, a supergroup, a forum, a monoforum and a community. Each case checked
eligibility, exactly one menu action and nonempty prepared content. Additional
checks covered service events, calls with `isService() == false`, gifts,
wallpaper records, full rich content, public-poll fallback for a broadcast,
deleted regular sources, deduplication and preservation of standalone text
with link previews. Cross-account checks covered identical message IDs,
ordered native/copy preparation, source logout, value-owned media snapshots,
and closing the share widget when its destination account disappears. Named
mentions were serialized using the destination account's access data or an
explicit user link. A viewer callback opened Share after its local source item
had been destroyed, retaining the full snapshot.

The UI check opened the actual history menu, triggered Share, selected Saved
Messages, and inspected the resulting buttons. Evidence from the synthetic
profile:

- [History context menu](assets/message-share/menu.png)
- [Share widget with selected recipient](assets/message-share/selected.png)

## Verification boundaries

The source/content matrix and request/transport tests do not prove delivery by
Telegram's live servers. Real paid charging, protected-media reference
acceptance, file-reference refresh and every independent screen have not been
exercised against a live account. File-reference errors get one refresh attempt;
subsequent rejection remains visible. No real message was sent by the tests.

The installed application was not replaced during the isolated validation.
Existing build/installation changes were preserved. Live account data and unrelated drafts were not read
or modified. Selections spanning different chats and accounts are supported.
A single-account selection uses that account; a mixed-account selection uses
the current account, copying content from the other accounts. Server acceptance
of media references copied between accounts remains part of live verification.

## Requested Release deployment (2026-09-22)

The Release build and message-share tests passed. The fully packaged bundle
replaced `/Applications/Forkgram.app`; its temporary backup was removed after
deployment. All 196 Mach-O files were
checked: no Homebrew or local-prefix dependencies remain. Public bundle access
and the deep strict signature passed. Source and installed executable SHA-256:
`dbfc54ba1333eef9b2becb0c817522682691badc3caee5f1e14e7a31f436c312`.

Forkgram was restarted in both logged-in sessions. Verification checked the
exact installed executable, user identity and a normal GUI window with the
same PID stable for at least 10 seconds: `wavecut` PID 61873 and non-owner
`wcard` PID 61945. Cross-user verification required entering the GUI audit
session as administrator before dropping to the destination user.
