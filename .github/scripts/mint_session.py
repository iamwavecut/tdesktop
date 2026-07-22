#!/usr/bin/env python3
"""
One-time LOCAL session minter for the release uploader account (never run in CI).

    pip install telethon
    TG_API_ID=<id> TG_API_HASH=<hash> python mint_session.py

Logs in interactively (phone + login code, plus 2FA password if set) and prints
a Telethon StringSession for the TG_SESSION secret. The session grants FULL
control of the account, so use a DEDICATED account that is admin of only the two
update channels (TG_FEED_CHANNEL, TG_FILES_CHANNEL) - not your personal one.
"""
import os
from telethon.sync import TelegramClient
from telethon.sessions import StringSession

api_id = int(os.environ.get("TG_API_ID") or input("api_id: ").strip())
api_hash = os.environ.get("TG_API_HASH") or input("api_hash: ").strip()

with TelegramClient(StringSession(), api_id, api_hash) as client:
    print("\n--- copy the line below into the TG_SESSION secret ---\n")
    print(client.session.save())
    print("\n--- keep it secret; anyone with it controls this account ---")
    me = client.get_me()
    print(f"\nlogged in as: {me.first_name} (@{me.username or 'no-username'}, id={me.id})")
