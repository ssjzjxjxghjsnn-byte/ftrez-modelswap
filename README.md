# FTREZ ModelSwap
 狗屎ai編寫 如若侵犯其問題 發送DMCA

本專案採用 GNU General Public License v3.0（GPL-3.0-only），詳見 [LICENSE](LICENSE)。第三方元件沿用各自目錄內的原始授權。

定位／輔助工具、`docs/`、構建產物與本機快取不納入 Git；保留 Gradle wrapper、構建入口與必要的資源構建／驗證腳本。

`app/src/main/cpp/self_model/` 中的下列必要內嵌資源隨原始碼納入 Git：

- `lobby_model.dll.bytes`
- `lobby_model.bundle`
- `lobby_model.build.json`
- `ftrez-toon.bundle`

資源原始碼及構建說明見 [managed/README.md](managed/README.md)。
