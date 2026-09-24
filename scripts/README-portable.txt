patX 免安装版（Windows）
========================

适用环境：不能安装白名单以外软件的 Windows（解压即用，不写注册表、
不需要管理员权限、不自带浏览器）。

目录结构
--------
  patx.exe                     主程序（静态链接，无 DLL 依赖）
  tools/web_dossier/           侧车脚本（CNIPA/EPO 降级时才需要 Python）
  data/deadline_rules/         中国专利期限规则集（62 条）
  patents.db                   首次运行时在本目录创建/使用

主包为纯 C++/Rust：USPTO 巡检（日常主路径）内置于 patx.exe，
零 Python、零捆绑浏览器。可选附加包 patX-cnipa-addon-python.zip
内含便携 Python：需要 CNIPA 登录接管或 EPO 查询时，把 zip 里的
python/ 文件夹解压到本目录即可启用。

使用方法
--------
1. 整个文件夹拷到 U 盘或目标机器任意可写目录（如桌面）
2. 双击 patx.exe 直接运行
3. 首次启动约 30 秒后自动开始后台巡检（默认每天一轮，
   可在 审查信息同步 -> 审查提醒设置 改为每 3/7 天）
4. 手动更新：国内申请列表选中案件 -> 【查询最新审查意见】

数据源说明
----------
- USPTO Global Dossier 公开 JSON API：无需账号，自动使用
- EPO Patent Register：公共页面（站点有人机验证时自动降级）
- CNIPA：需要登录时才提示，复用系统已安装的 Edge/Chrome
  （接管模式，不安装捆绑浏览器、不读取密码；需先装可选 Python 附加包）

注意事项
--------
- 数据库 patents.db 就在本目录，备份=复制该文件
- 换电脑迁移：整个文件夹复制过去即可（含数据库与设置）
- 期限计算内置中国法定节假日仅按周末顺延；法定节假日表
  可后续注入 data/deadline_rules/
