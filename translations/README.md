The UI catalog supports English (source text), Simplified Chinese (`zh_CN`),
Traditional Chinese (`zh_TW`), Japanese (`ja`), German (`de`), French (`fr`),
Spanish (`es`), and Korean (`ko`). Every non-English catalog contains the same
shared annotation, capture-mode, background, and action vocabulary. Unknown
source strings and unsupported languages use the original English text.

Language selection: an explicit `language` setting or `--language` option wins;
`auto`, `system`, and an empty value select `HYPRCAPTURE_LANGUAGE`, then the system
locale. Regional forms such as `de_DE` use their base language. `zh_Hant`, `zh_TW`,
`zh_HK`, and `zh_MO` use Traditional Chinese; other Chinese forms use Simplified
Chinese. Qt's installed `qtbase` translations are loaded when available to localize
standard dialogs; HyprCapture labels are embedded and require no runtime assets.

Edit `ui_catalog.json`, then run `python3 translations/generate_catalog.py` to
regenerate `src/ui/translations_catalog.hpp`. The generator checks that all
catalogs contain the same nonempty messages. Runtime source keys must remain
English; keep machine-readable option values separate from translated labels.

Some shared annotation labels were adapted from the translations of
[omarchy-screenshot](https://github.com/manateelazycat/omarchy-screenshot), commit
`a9b857fb17dfe7b9ad73b0edec3554914e3079bc`, by manateelazycat and contributors.
Those translations and this repository are licensed under GNU GPL version 3
(see the repository's `LICENSE`). This adapted catalog adds HyprCapture-specific
capture and editor labels and Korean translations. It does not include or claim
complete support for all languages in the upstream catalog.
