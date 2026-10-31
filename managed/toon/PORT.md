# FTRez Eevee 風格動態三渲二

將 Soyo Blender 預覽的三段 ColorRamp 概念移植為 Unity URP 的動態 Shader。不是將 Eevee 節點直接匯出，也沒有把固定陰影烘焙到模型貼圖。

## 效果

Shader 名稱 `FTREZ/EeveeToon`。先加總主光、額外光與球諧環境光，再依亮度分成暗／中／亮三段（門檻 .32/.64），使用導數平滑邊界。保留光源色調、距離衰減、實時陰影和低照度變暗。臉部採較淺色帶；runtime 識別 Soyo_face.007、Soyo_eye.007 和 Soyo_Material.007 等實際材質名。

支援主光一般／級聯／螢幕陰影，額外光、Forward/Forward+、霧、alpha cutout、雙面剔除設定、ShadowCaster、DepthOnly、DepthNormals。沿用現有獨立描邊方式。沒有新增完整半透明、SDF 臉影或頭髮高光。

這是 Eevee 風格近似；遊戲曝光、燈光強度、後處理和色彩空間可能使畫面不同於 Blender 預覽，需要實機調校。

## 編譯

- Unity 2022.3.62f3（96770f904ca7），與遊戲 globalgamemanagers 版本一致。
- Editor 隨附 URP 14.0.12；遊戲包含 URP，但其確切 package patch／自訂修改尚未確認。
- Android GLES3 + Vulkan；需 Android Build Support 和有效 Unity 授權。
- 精簡專案必須包含 com.unity.modules.assetbundle，否則可能缺少載入用資產索引。

從儲存庫根目錄執行：

```bash
python -X utf8 lsp-ui/model_tools/toon/build.py
```

可用 `--unity` 指定 Unity.exe，`--project` 指定獨立編譯專案。腳本將本目錄 Assets 複製到編譯專案，不修改模型。BuildFTRezToon.Configure 建立 URP 設定，保留使用／不使用陰影與额外光的變體，Build 嚴格編譯後實際載入 Bundle 驗證 Shader 名稱。

產物在 `build/unity-eevee/Build/Android/`：`ftrez-toon.bundle`、manifest、build-info.txt。沿用原 Shader 檔案路徑，保持 runtime LoadAsset 路徑相容。

## 接入

Bundle 複製到 `lsp-ui/src/main/cpp/self_model/ftrez-toon.bundle` 後由 CMake 嵌入 native library。保留 Realistic、Realistic + outline、Toon 原有選項及編號；新增 Eevee Toon（編號 3）載入；失敗顯示 EeveeToon bundle unavailable 並回退舊 Shader。Toon（編號 2）直接使用原本 fast-cartoon Shader，預設及既有設定保持 Toon。無需重新匯出 Soyo GLB。

## 驗證與限制

本次 Unity Android 編譯成功，Unity LoadFromFile／LoadAsset 驗證成功；另以 UnityPy 確認資產索引、Shader 名稱、GLES3/Vulkan 二進位與 opaque/clipped 材質。既有模型 ctest 4/4 通過。Android APK 編譯成功，逐位元確認 Bundle 已嵌入 APK 的 ARM64 native library。

未安裝到手機，未進行遊戲畫面或 Android GPU 實測。室內外、第一人稱、本人主頁、光照／陰影品質切換及透明髮絲需要實機檢查；不能把 Blender 預覽當作本次遊戲效果證據。

## 上游與授權

以 NiloCat 的公開 URP 教學 Shader 為結構基礎，改寫光照方程、陰影／額外光迴圈與材質參數。
來源：https://github.com/ColinLeung-NiloCat/UnityURPToonLitShaderExample
commit：fbe276e0df1d1f9379e13581c20b6a080a1a6977
MIT 授權保留於 Assets/FTRezToon/LICENSE。不是 NiloToon 商業完整版。


## Target color space correction

The target `globalgamemanagers` PlayerSettings reports `m_ActiveColorSpace = 1` (Linear). The initial build project used Gamma (0). `BuildFTRezToon.Configure` now explicitly selects `ColorSpace.Linear`, and build-info records this setting. URP `SampleSH9` converts ambient lighting to sRGB when `UNITY_COLORSPACE_GAMMA` is defined; this conversion is inappropriate for the target linear pipeline and can push the toon ramp toward brighter bands. The correction preserves the shading equations and model geometry. The folder renderer update is delivered separately from the previously embedded Eevee bundle, so it works with the existing Custom renderer APK. Prior bundles remain in `build/model-resources/Custom-Renderer-Gamma-Backup` and existing ZIPs. In-game appearance still requires comparison.
