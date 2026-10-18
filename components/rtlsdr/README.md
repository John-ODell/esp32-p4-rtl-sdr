# rtlsdr component

- `blog/` is librtlsdr from the RTL-SDR Blog fork (rtlsdrblog/rtl-sdr-blog, commit aed0ea1,
  2026-03-22), which adds the V4's R828D + upconverter-free HF handling. GPL-2.0-or-later, see
  `blog/COPYING` and the file headers.
  The files are copied unmodified.
- `shim/` holds a libusb-1.0 subset written for this project on the ESP-IDF USB Host Library.
- `include/rtlsdr_usb.h` brings up the USB host with hub support, because the kit's Type-A ports
  sit behind a CH334F hub.

Because librtlsdr is under the GPL, firmware that links this component is too. This repository uses
GPL-3.0-or-later (librtlsdr allows "any later version"), since GPLv3 can be combined with ESP-IDF's
Apache-2.0 code and GPLv2 cannot.
