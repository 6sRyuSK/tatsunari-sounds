# visage コア API リファレンス(pin `20de5946` / examples 全読から抽出)

このリポジトリが FetchContent する VitalAudio/visage の実用 API 面。原典を読み直す
必要が出たら、configure 済みビルドの `<build>/_deps/visage-src/`(examples 含む)に
ピン版チェックアウトがある。座標は既定で**logical px**(DPI スケールは canvas が適用)。
色は `0xAARRGGBB`。

## アプリ/ウィンドウ (`<visage/app.h>`)

```cpp
visage::ApplicationWindow app;               // ApplicationEditor(=Frame) 派生
app.onDraw() = [&](visage::Canvas& c) {...}; // または派生して draw() override
app.setTitle("...");
app.show(800, 600);                          // Dimension も可: show(80_vmin, 60_vmin)
app.showMaximized();                         // モバイル/フルスクリーン系
app.show(parent_ptr);                        // ★親窓に embed (CLAP: clap_window->ptr)
app.runEventLoop();                          // スタンドアロンのみ。plugin では呼ばない
```

- `using namespace visage::dimension;` で単位リテラル: `_px`(logical) `_npx`(native)
  `_vw` `_vh` `_vmin` `_vmax`(親比%)。`Dimension::min/max(a,b)` で合成可。
- `ApplicationEditor`(窓なしの最上位 Frame): `onShow()/onHide()/onCloseRequested()/
  onWindowContentsResized()`(CallbackList — `=` 代入か `+=` 追加)、
  `takeScreenshot()`、`setFixedAspectRatio(bool)`(**現在の**縦横比を捕捉)、
  `setMinimumDimensions(w,h)`、`adjustWindowDimensions(&w,&h,horiz,vert)`、
  `window()`(→ `posixFd()` / `processPluginFdEvents()`)。
- `ApplicationWindow` 追加分: `setNativeWindowDimensions(w,h)` /
  `setWindowDimensions(Dimension...)`、`setWindowDecoration(Window::Decoration::Client)`
  (自前タイトルバー)、`isShowing()`、`hide()/close()`。
- macOS だけ logical (`width()`/`setWindowDimensions`)、他は native
  (`nativeWidth()`/`setNativeWindowDimensions`) を CLAP サイズ報告に使う
  (upstream ClapPlugin example の #if __APPLE__ 分岐)。
- マルチウィンドウ可: 別 `ApplicationWindow` メンバを `show(10_vw, 10_vh, 400_px,
  300_px)` で出す(MultiWindow example)。ヘッドレスは `setWindowless(w,h)`。

## Frame (`visage_ui/frame.h`)

ツリー・入力・再描画の単位。仮想 override と CallbackList (`onXxx()`) の両方がある
(既定コールバックが仮想関数を呼ぶ; ラムダ合成なら `frame.onDraw() = ...`)。

| 分類 | API |
|---|---|
| ライフサイクル | `init()` `draw(Canvas&)` `destroy()` `resized()` `visibilityChanged()` `hierarchyChanged()` `dpiChanged()` `focusChanged(is,clicked)` |
| ツリー | `addChild(Frame*/&/unique_ptr, make_visible=true)` `removeChild` `removeAllChildren` `parent()` `children()` `findParent<T>()` `frameAtPoint(p)` `topParentFrame()` |
| 配置 | `setBounds(x,y,w,h)`(float, 親 local) `bounds()` `localBounds()` `x() y() width() height() right() bottom()` `nativeWidth()...`(物理px) `positionInWindow()` `relativeBounds(other)` `aspectRatio()` |
| 可視/描画 | `setVisible(b)` `isVisible()` `setDrawing(b)` `setOnTop(b)` `redraw()` `redrawAll()` |
| 入力制御 | `setIgnoresMouseEvents(ignore, pass_to_children)` `setReceiveChildMouseEvents(b)`(子のイベントも受ける — `e.event_frame` で発生元判別) `setAcceptsKeystrokes(b)` `requestKeyboardFocus()` `hasKeyboardFocus()` |
| マウス | `mouseEnter/Exit/Down/Up/Move/Drag(const MouseEvent&)` `mouseWheel → bool` |
| キー/テキスト | `keyPress/keyRelease(const KeyEvent&) → bool`(処理したら true) `receivesTextInput()` `textInput(str)` |
| レイヤ効果 | `setPostEffect(PostEffect*)` `setBackdropEffect(...)`(背後をぼかす glass) `setBlurRadius(px)` `setMasked(b)`(MaskAdd/Remove 用) `setAlphaTransparency(a)`(グループ透過) `setCached(b)` — どれも中間レイヤを作る(コスト有) |
| D&D | `isDragDropSource()` `startDragDropSource()` `cleanupDragDropSource()` / `receivesDragDropFiles()` `dragDropFileExtensionRegex()` `dragFilesEnter/Exit` `dropFiles(paths)` |
| その他 | `setCursorStyle(MouseCursor::Arrow/MultiDirectionalResize/…)` `setCursorVisible(b)` `setMouseRelativeMode(b)` `readClipboardText()/setClipboardText()` `paletteValue/paletteColor`(未使用系, SKILL.md gotcha 6) |

**再描画モデル(最重要)**: 描画は on-demand。`redraw()` した Frame だけ次フレームで
`draw()` が呼ばれる。連続アニメーションは `draw()` の**末尾で自分の `redraw()`** を
呼んで自走させる(Showcase の bars/shapes、家の SpectrumView/RsSuppressionCurveView)。
静止 widget はコストゼロで、スクリーンショットも決定的になる。

## MouseEvent / KeyEvent (`visage_ui/events.h`)

```cpp
e.position            // ★frame-local ヒット座標(これを使う)
e.relativePosition()  // 相対モード用の移動デルタ — ヒットに使うと (0,0) に化ける
e.windowPosition()    // window px(オーバーレイ座標変換用)
e.event_frame         // イベント発生 Frame(receiveChildMouseEvents 併用)
e.isLeftButton()/isMiddleButton()/isRightButton()   // 押されたボタン(button_id)
e.isLeftButtonCurrentlyDown()                        // 現在押下状態(button_state)
e.isShiftDown()/isAltDown()/isCtrlDown()/isCmdDown()/isMainModifier()
e.repeatClickCount()  // 1=シングル, 2=ダブル(判定は >= 2)
e.shouldTriggerPopup()// 右クリック or macOS Ctrl+左
e.wheel_delta_y / e.precise_wheel_delta_y  // ホイール(両方見る), hasWheelMomentum()
```

`KeyEvent`: `keyCode()`(`visage::KeyCode::Z` 等) + 同じ modifier 群 + `isRepeat()`。
タイマは `visage::EventTimer`(`startTimer(ms)` + `timerCallback()`)、UI スレッドへの
ポストは `visage::runOnEventThread(fn)`。

## Canvas (`visage_graphics/canvas.h`)

状態機械: ブラシを set してから shape を積む。フレーム座標系は呼び出し元 Frame の local。

```cpp
canvas.setColor(0xffff7a6b);                 // solid
canvas.setColor(visage::Color(argb));
canvas.setColor(brush);                      // 下記 Brush
canvas.setBlendMode(visage::BlendMode::Alpha /*Add Sub Mult MaskAdd MaskRemove*/);
canvas.saveState(); ... canvas.restoreState();
canvas.setPosition(dx, dy);                  // 原点シフト(state に累積)
canvas.setClampBounds(x,y,w,h); canvas.trimClampBounds(...);  // クリップ
canvas.time(); canvas.deltaTime(); canvas.frameCount();       // アニメ時計
canvas.setNativePixelScale(); canvas.setLogicalPixelScale();  // px スナップ描画用
canvas.debugInfo();                          // FPS 等の文字列(Showcase の Shift+Cmd+D)
```

### shape 呼び出し(引数は x, y = 左上)

| call | 備考 |
|---|---|
| `fill(x,y,w,h)` / `rectangle(...)` | 矩形塗り |
| `rectangleBorder(x,y,w,h,thickness)` | 枠 |
| `roundedRectangle(x,y,w,h,rounding)` / `roundedRectangleBorder(..., thickness)` | 角丸 |
| `roundedRectangleShadow(x,y,w,h,rounding,blur)` | ソフト影(本体より先に描く) |
| `leftRoundedRectangle` 他 top/bottom/right | 片側だけ角丸 |
| `circle(x,y,diameter)` | ★第3引数は直径、x,y は外接左上 |
| `fadeCircle(x,y,d,fade_px)` / `ring(x,y,d,thickness)` | ぼかし円 / 輪 |
| `squircle(x,y,w,power=4)` / `superEllipse(x,y,w,h,power)` | スクワークル |
| `arc(x,y,d,thickness,center_rad,half_aperture,rounded=false)` | ★中心角±半開き規約; 画面角は +90° ずれる — 家では `fillArcBand` 経由 |
| `segment(ax,ay,bx,by,thickness,rounded)` | 線分(針・ディバイダ) |
| `quadratic(ax,ay,bx,by,cx,cy,thickness)` | 2次ベジェ線 |
| `triangle(ax,ay,bx,by,cx,cy)` / `roundedTriangle(...)` / `triangleLeft/Right/Up/Down(x,y,w)` | 三角(矢印) |
| `diamond(x,y,w,rounding)` | ひし形 |
| `text(str, font, Font::kCenter, x,y,w,h [,Direction])` | 矩形内配置。`Font::Justification`: kLeft/kRight/kCenter/kTop/kBottom + kTopLeft 等の合成 |
| `fill(path [,x,y[,w,h]])` | `visage::Path` 塗り(path-fill アトラス経由 — 大きい path で汚染注意) |
| `stroke(path, x,y,w,h, width, Join, EndCap, dash_array, dash_offset)` | path 線 |
| `svg(data,size,x,y,w,h)` / `svg(EmbeddedFile,...)` | SVG(現在ブラシで着色可) |
| `image(EmbeddedFile/data, x,y,w,h)` | PNG 等 |
| `graphLine(GraphData, x,y,w,h,thickness)` / `graphFill(..., fill_center)` | 折れ線/面(GraphLine widget の裏側) |
| `shader(Shader*, x,y,w,h)` | カスタムシェーダ矩形 |

### Brush / Gradient (`visage_graphics/gradient.h`)

```cpp
visage::Brush::solid(argb)
visage::Brush::vertical(colorA, colorB)      // 縦グラデ(横は horizontal)
visage::Brush::linear(gradient, fromPoint, toPoint)
visage::Brush::radial(gradient, centerPoint, radius)  // (Color, Color, center, rx, ry) 版も家で使用
visage::Gradient(c1, c2, ...);               // 多段
visage::Gradient::fromSampleFunction(n, [](float t){ return visage::Color(...); })
gradient.setRepeat(b); gradient.setReflect(b);
visage::Gradient::kViridis                   // 組み込みカラーマップ
```

### Path (`visage_graphics/path.h`)

`moveTo lineTo bezierTo(quad/cubic) close addCircle addRoundedRectangle`、
`path.stroke(width, Path::Join::Round, Path::EndCap::Round, {dash…}, dash_offset)` は
新しい Path を返す。`scaled(s)` / `translated(dx,dy)` / `length()` / `boundingBox()`。
SVG 由来のグリフは `Icons.h` の写経規約(C→bezierTo, M/L→moveTo/lineTo)で移す。

## Font / 埋め込みリソース

- `visage::Font(sizePx, data, dataSize)` / `(sizePx, EmbeddedFile)` — **ビルド時埋め込み**
  のバイト列から作る(実行時ファイルロード無し)。家では必ず
  `factory_ui_visage::regularFont/boldFont(px)` 経由。
- CMake: `add_embedded_resources(target "header.h" "name::space" "${FILES}")` が
  .ttf/.svg/.png → `EmbeddedFile`(`name::space::File_ext`)を生成。シェーダは
  `visage_embed_shaders(target "header.h" "ns" "${SC_FILES}")`(bgfx .sc を build 時に
  同梱 `shaderc` でクロスコンパイル; wasm は ESSL 100_es)。

## Layout(flex; `visage_ui/layout.h`)

手動 `resized()` + `setBounds` が家の標準(RsEditor)。ギャラリー/ツール類は flex 可:

```cpp
frame.setFlexLayout(true);                 // = layout().setFlex(true)
frame.layout().setPadding(10_px);          // Margin も同型(4辺 or Left/Right/Top/Bottom)
frame.layout().setFlexGap(8_px);
frame.layout().setFlexRows(false);         // true=縦積み(既定) false=横並び
frame.layout().setFlexWrap(true);          // setFlexWrapReverse / setFlexReverseDirection
child.layout().setWidth(100_px);           // setHeight / setDimensions
child.layout().setFlexGrow(1.0f);          // setFlexShrink
frame.layout().setFlexItemAlignment(visage::Layout::ItemAlignment::Center); // Stretch(既定)/Start/End
frame.layout().setFlexWrapAlignment(visage::Layout::WrapAlignment::Stretch);
```

`ScrollableFrame`(`visage_ui/scroll_bar.h`): `addScrolledChild(f)` +
`scrollableLayout()`(スクロール面の layout)。

## PostEffect(視覚効果; 全て opt-in のレイヤコスト)

```cpp
visage::BloomPostEffect bloom; bloom.setBloomSize(30); bloom.setBloomIntensity(2);
frame.setPostEffect(&bloom);
visage::BlurPostEffect blur; blur.setBlurRadius(40);
glass.setBackdropEffect(&blur);            // 背後を磨りガラス化(+MaskAdd で形を抜く)
auto fx = std::make_unique<visage::ShaderPostEffect>(resources::shaders::vs_custom,
                                                     resources::shaders::fs_warp);
fx->setUniformValue("u_zoom", 1.05f);      // uniform 注入(Showcase のオーバーレイズーム)
```

## upstream examples 索引(`<build>/_deps/visage-src/examples/`)

| example | 学べること |
|---|---|
| `Basic` | 最小: ApplicationWindow + onDraw + show + runEventLoop |
| `Layout` | flex(padding/gap/wrap/reverse/grow)と `_px` リテラル |
| `MouseEvents` | mouse override 一式、相対モード、`shouldTriggerPopup`、PopupMenu |
| `Paths` | Path 構築、stroke/dash(`canvas.time()` でダッシュ流し) |
| `Gradients` | Brush::linear/radial、Gradient 生成 4 種、点ドラッグの当たり判定 |
| `BlendModes` | Add/Sub、`setMasked`+MaskRemove/MaskAdd、`setAlphaTransparency` |
| `Bloom` | BloomPostEffect、GraphLine 大量点、`palette()->setColor` 連携 |
| `PostEffects` | ShaderPostEffect 切替、BlurPostEffect、backdrop(磨りガラス)、`event_frame` 判別ドラッグ、カーソル切替 |
| `LiveShaderEditing` | ShaderEditor + ShaderPostEffect のライブ編集 |
| `MultiWindow` | サブ ApplicationWindow、onShow/onHide、`+=` コールバック |
| `BringYourOwnWindow` | Renderer/Canvas を素の Window に直結(最低層; 家では不使用) |
| `Showcase` | ScrollableFrame ギャラリー、Animation<float>、UndoHistory、テーマ palette、D&D、debugInfo |
| `ClapPlugin` | clap-helpers 上の gui 拡張実装(家の RsClapEditor の原型; Apple の logical/native 分岐込み) |

家で**使わない**もの(SKILL.md gotcha 6): `PopupMenu`、`UiButton`/`ToggleTextButton` 系、
`VISAGE_THEME_COLOR` + `Palette` テーマ機構(家は factory_ui_visage::Theme)。
`visage::Animation<float>`(時間補間)と `UndoHistory` は使っても良いが、param undo は
`factory_params::UndoStack` が正。
