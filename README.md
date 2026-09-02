# hd-esp32-s3

Firmware for an ESP32-S3 desktop display with a 300×400 reflective LCD. It combines a clock,
indoor conditions, weather, battery status, personal notes, and AI usage limits in one always-on
device.

## What you can do

- See two time zones, indoor temperature and humidity, battery status, and weather for up to two
  cities on the LCD.
- Cycle between the overview, a shared Google Doc, and a to-do list.
- View sensor and battery history from a phone or computer on the same network.
- Configure Wi-Fi, cities, time zones, update intervals, and integrations in a web UI.
- Show Claude and Codex usage limits.
- Install released firmware from the web UI or upload a local build over USB or Wi-Fi.

## Set up the device

1. [Build and flash the firmware](docs/build-and-release.md). The first installation must use USB.
2. Insert a microSD card if you want settings, cached web assets, to-do items, history, and release
   information to survive a restart.
3. Start the device. If it has no saved Wi-Fi network, join the open **hd-esp32-setup** network from
   a phone or computer and use the captive page to add one.
4. On the same LAN, open [http://esp32.local](http://esp32.local). If mDNS is unavailable, use the IP
   address shown by the device or in the serial log.
5. Open **Configuration** to choose time zones and cities and, if wanted, configure the Google Doc,
   Claude usage, Codex usage, refresh intervals, and firmware source.

The web UI has no user login, and integration credentials are stored on the SD card. Keep the device
on a trusted network and treat the card as sensitive.

## Use it

- Press **KEY** to request fresh weather, Claude usage, Codex usage, and Google Doc content. A short
  sound acknowledges the press and a longer sound plays when that refresh finishes.
- Press **BOOT** to cycle the LCD view. If a Google Doc update popup is visible, BOOT dismisses it
  instead.
- Open **Dashboard** in the web UI to edit the to-do list and inspect temperature, humidity, voltage,
  and battery history.
- Open **Configuration** to change device settings or install a published firmware release.
- Read the current values from scripts with `GET http://esp32.local/stats`.

Scheduled updates, startup, and Wi-Fi reconnection are silent. Only a refresh requested with the KEY
button plays the refresh sounds.

## Optional integrations

- **Weather:** add cities by name. The first two in the saved order appear on the LCD.
- **Google Doc:** provide a normal Docs link and share the document as “Anyone with the link.” New or
  edited lines appear in an on-screen popup.
- **Claude usage:** paste the browser cookie, or enter the organization ID and session key.
- **Codex usage:** relay the access token from a logged-in Codex computer. Follow the
  [Codex usage relay guide](docs/codex-usage-relay.md).

## More documentation

- [Build, flash, and release](docs/build-and-release.md)
- [Codex usage relay](docs/codex-usage-relay.md)
- [Firmware architecture](docs/architecture.md)
- [Licensing notes](docs/licensing.md)
- [Contributor and hardware reference](CLAUDE.md)

## License

MIT — see [LICENSE](LICENSE) and the [licensing notes](docs/licensing.md).
