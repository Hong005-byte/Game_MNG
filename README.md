# Tycoon Idle

一个用 C++20 + SFML 3 写的俯视角小镇经营 / 人生模拟游戏。一边在小镇里走动、盖产业、跑腿做小游戏,一边有一套完整的经济/生活模拟系统在背后持续运行——就算不在游戏窗口里,回来时也会自动补算离线期间的进度。

支持中英双语(启动时选择),两种玩法界面:经典控制台菜单,或者可以用 WASD/方向键+鼠标走动点击的图形化小镇视角。

## 下载

去 [Releases](https://github.com/Hong005-byte/Game_MNG/releases) 页面下载最新的 `TycoonIdle-vX.X.X-win64.zip`,解压后直接运行 `Game Mng.exe`——是免安装的便携版,不需要额外装任何运行库。游戏内建了自动更新检测,有新版本时会提示。

仅支持 Windows。

## 玩法概览

- **经营**:一棵完整的产业树,从原材料(农场、矿场、渔港……)到加工品,雇员工、请工人、专精产业,滚雪球式发展经济。
- **人生模拟**:角色会变老、会饿、会累、会生病——睡觉恢复体力、吃饭恢复饥饿、生病了要看医生,长期忽视是会死的。角色去世后传承点数会留给下一代,带着小幅永久加成重新开始。
- **四季系统**:春夏秋冬每季 60 个游戏日(2 个月一季,一年 8 个月),不只是好看——春天不容易生病、夏天体力消耗更快、秋天全产业产出小幅提升、冬天饥饿消耗更快且更容易生病;换季时还有色块过场动画和对应的氛围粒子(飘雪、落叶、花瓣、热浪)。
- **市场**:低买高卖,价格会随时间和你自己的交易/生产波动,部分商品在当季需求旺盛时能卖更高价。
- **成就 & 传承**:20+ 个成就,解锁给现金奖励;死亡后传承点数可以买永久加成,其中一条能专门削弱冬季的额外惩罚。
- **暂停菜单**:随时按 Esc 存档、调音量、切换分辨率/全屏、自定义按键。

详细玩法说明在游戏内的"How to Play / 玩法"面板里,比这里写得全。

## 从源码编译

需要 Visual Studio 2022+(含 C++ 桌面开发工作负载)和 [vcpkg](https://github.com/microsoft/vcpkg)。

```bash
git clone https://github.com/Hong005-byte/Game_MNG.git
cd Game_MNG
```

依赖(SFML 3)通过 `Game Mng/vcpkg.json` 的 manifest 模式管理,用 Visual Studio 打开 `Game Mng.slnx` 编译时会自动安装,首次编译会因为要拉取/编译 SFML 而比较慢。也可以手动跑一次：

```bash
vcpkg install --triplet x64-windows --x-manifest-root="Game Mng"
```

编译产物在 `Game Mng/x64/Release/`(或 `Debug/`),同目录下已经有编译脚本会自动把所需的运行时 DLL 拷贝过去。

## 项目结构

- `Game.h/.cpp` —— 核心经济/人生模拟状态机,两套界面(控制台菜单 / 图形化世界)共用同一份状态
- `GameWorld.h/.cpp` —— 图形化小镇视角:渲染、输入、小游戏、所有窗口内的 UI 面板
- `Business.h/.cpp`、`Market.h/.cpp`、`Staff.h/.cpp`、`Life.h/.cpp`、`Events.h/.cpp`、`Achievements.h/.cpp` —— 各自独立的子系统
- `Localization.h/.cpp` —— 中英文字符串表
- `SaveManager.h/.cpp`、`Settings.h/.cpp` —— 存档(按角色)与玩家偏好(音量/按键/分辨率,跨存档共用)的持久化
- `UpdateChecker.h/.cpp`、`Version.h` —— 启动时后台查询 GitHub Releases 是否有新版本

## 截图

_(待补充)_
