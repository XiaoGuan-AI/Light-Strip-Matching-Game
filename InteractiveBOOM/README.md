# Interactive BOM 双击亮灯

本目录新增了两份辅助文件：

- `interactive_bom_bridge.js`
  负责在嘉立创导出的 Interactive BOM 页面中监听双击，并把识别出的 LCSC 料号发送到分拣箱的 `/api/find` 接口。
- `inject_interactive_bom_bridge.py`
  用来给新导出的 Interactive BOM HTML 自动注入上面的桥接脚本。

## 当前文件

`InteractiveBOM_PCB130_2026-3-12.html` 已经注入桥接脚本，可以直接打开使用。

更推荐通过本地 HTTP 打开，而不是直接用 `file://`。

## 最省事的用法

以后你每次新导出 BOM，只要把 HTML 放到本目录，然后直接双击：

```bat
open_latest_bom.bat
```

它会自动完成：

- 找到本目录里最新导出的 `InteractiveBOM*.html`
- 自动注入 `interactive_bom_bridge.js`
- 启动本地 HTTP 服务
- 自动打开浏览器到对应 BOM 页面

## 后续新导出文件

把新的 HTML 放到本目录后，在本目录执行：

```bash
python inject_interactive_bom_bridge.py
```

也可以只处理指定文件：

```bash
python inject_interactive_bom_bridge.py "InteractiveBOM_*.html"
```

如果你想手动指定文件，也可以继续用原来的方式处理。

## 本地 HTTP 打开

在 Windows 下可直接双击：

```bat
start_local_http.bat
```

如果你想直接打开“最新导出的 BOM”，更推荐双击：

```bat
open_latest_bom.bat
```

或者手动执行：

```bash
python serve_interactive_bom.py
```

默认会打开：

```text
http://127.0.0.1:8000/InteractiveBOM_PCB130_2026-3-12.html
```

`open_latest_bom.bat` 会自动把这里的文件名替换成当前最新导出的那个 BOM。

## 使用说明

1. 打开 Interactive BOM 页面。
2. 桥接脚本默认会优先连接固定地址 `http://192.168.3.75:32323`。
3. 如果你现场使用的不是这个地址，再点击右下角“设置地址”手动修改。
4. 双击包含 LCSC 料号的 BOM 行，例如 `C25882`。
5. 分拣箱会调用 `/api/find`，若数据库中存在该料号，则对应灯位亮起。

设备地址会保存到浏览器 `localStorage` 的 `componentSorterBaseUrl` 中，后续同一浏览器可直接复用。

如果当前保存的地址不可用，桥接脚本会自动回退尝试常用候选地址，例如 `192.168.3.75:32323` 和 `192.168.3.238:32323`，并在连接成功后自动记住新的可用地址。

如果表格当前行里没有直接显示 LCSC 料号，桥接脚本还会尝试读取导出页内置的 `window.files.bom_merge` 数据做兜底匹配，以提升不同导出版本下的识别率。

## 新增交互增强

- 右下角状态条会显示当前联动状态、查找结果和时间。
- 最近查找面板会保留最近 8 个料号，点击即可再次触发亮灯，并用颜色区分“已找到 / 未找到 / 请求异常”。
- 如果当前 BOM 行匹配到多个可能料号，会弹出选择框供你手动选择。
- 双击后当前 BOM 行会自动高亮，方便你在贴片时持续盯住当前操作对象。

## 注意

如果当前 BOM 页面出现白屏，先确认导出的 HTML 文件本身是完整的，再运行 `inject_interactive_bom_bridge.py` 重新注入桥接脚本。对于大体积 Interactive BOM，更建议重新导出原始 HTML 后再按本目录流程处理，而不是直接在已损坏的文件上继续修补。
