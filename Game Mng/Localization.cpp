#include "Localization.h"
#include <unordered_map>

Language Localization::current = Language::English;

namespace {
    const std::unordered_map<std::string, std::pair<std::string, std::string>> kTable = {
        // Startup / mode selection
        { "mode_title",  { "Choose how to play:", "选择游玩方式:" } },
        { "mode_1",      { "1) Classic console menu", "1) 经典控制台菜单" } },
        { "mode_2",      { "2) Walk-around town (WASD/arrows + click, graphical)", "2) 俯视角小镇(WASD/方向键+点击,图形化)" } },
        { "mode_prompt", { "Choice: ", "选择: " } },
        { "update_available_prefix", { "A new version (v", "发现新版本(v" } },
        { "update_available_mid",    { ") is available! Download it here: ", ")!下载地址: " } },
        { "update_available_suffix", { "\n", "\n" } },
        { "update_banner_prefix",         { "New version available: v", "发现新版本: v" } },
        { "update_banner_open_button",    { "Get it", "去下载" } },
        { "update_banner_dismiss_button", { "X", "X" } },

        // Save-slot picker (runs after language selection, before mode selection)
        { "save_select_title",   { "Select a Save:", "选择存档:" } },
        { "save_new_game",       { "New Game", "新建存档" } },
        { "save_slot_gen",       { "Gen ", "第 " } },
        { "save_slot_last_played", { "last played ", "上次游玩 " } },
        { "save_slot_age_suffix",  { "yo", "岁" } },
        { "save_slot_prompt",    { "Choice: ", "选择: " } },
        { "save_delete_hint",    { "Enter D followed by a slot number to delete it (e.g. D3).", "输入 D+编号可以删除该存档(例如 D3)。" } },
        { "save_delete_confirm_prefix", { "Delete save \"", "确定要删除存档\"" } },
        { "save_delete_confirm_suffix", { "\"? (1 = yes, 0 = no): ", "\"吗?(1 = 是,0 = 否): " } },
        { "save_deleted_prefix", { "Deleted save \"", "已删除存档\"" } },
        { "save_deleted_suffix", { "\".\n", "\"。\n" } },
        { "save_delete_failed",  { "Could not delete that save file.\n", "无法删除该存档文件。\n" } },
        { "save_name_prompt",    { "Name this save (leave blank for a default name): ", "为存档命名(留空使用默认名称): " } },
        { "save_created_prefix", { "Created save \"", "已创建存档\"" } },
        { "save_created_suffix", { "\".\n", "\"。\n" } },
        { "save_loaded_prefix",  { "Loading save \"", "正在读取存档\"" } },
        { "save_loaded_suffix",  { "\"...\n", "\"……\n" } },
        { "difficulty_prompt",   { "Choose a difficulty:\n1) Easy (more starting cash, gentler upkeep and disasters)\n2) Normal\n3) Hardcore (less starting cash, harsher upkeep and disasters)\nChoice: ",
                                    "选择难度:\n1) 轻松(初始资金更多,维护费和天灾更温和)\n2) 普通\n3) 硬核(初始资金更少,维护费和天灾更狠)\n选择: " } },

        // Window / HUD
        { "window_title",  { "Tycoon Idle - Town", "经营人生 - 小镇" } },
        { "hud_generation", { "Generation", "世代" } },
        { "hud_cash",       { "Cash", "现金" } },
        { "hud_age",        { "Age", "年龄" } },
        { "hud_day_prefix", { "Day ", "第" } },
        { "hud_day_suffix", { "", "天" } },
        { "hud_years",      { "yrs", "岁" } },
        { "hud_season_prefix", { "Season: ", "季节: " } },
        { "season_spring",  { "Spring", "春季" } },
        { "season_summer",  { "Summer", "夏季" } },
        { "season_autumn",  { "Autumn", "秋季" } },
        { "season_winter",  { "Winter", "冬季" } },
        { "season_effect_spring", { "(less sickness)", "(不易生病)" } },
        { "season_effect_summer", { "(+30% energy drain)", "(体力消耗+30%)" } },
        { "season_effect_autumn", { "(+8% output)", "(全产业产出+8%)" } },
        { "season_effect_winter", { "(+30% hunger drain, more sickness)", "(饥饿消耗+30%,更易生病)" } },
        { "hud_days_until_season_prefix", { " -- ", " ——还剩 " } },
        { "hud_days_until_season_suffix", { " day(s) left", " 天换季" } },
        { "hud_help",       { "{MOVE}/arrows to move (hold Shift to sprint), {E} or click a building to interact, {U} to quick-upgrade one nearby, {M} to toggle the minimap. Walk off the edge to travel between areas.",
                               "{MOVE}/方向键移动(按住Shift加速),{E}键或点击建筑互动,{U}键可快速升级附近的产业,{M}键切换小地图。走到地图边缘可以前往其他区域。" } },
        { "entering",       { "[Entering ", "[进入 " } },
        { "entering_short", { "Entering ", "进入 " } },
        { "achievements_button", { "Achievements", "成就" } },
        { "howtoplay_button", { "How to Play", "玩法" } },
        { "recipebook_button", { "Recipes", "配方图鉴" } },
        { "recipebook_header", { "Recipe Book -- click an item to see what it takes", "配方图鉴——点击物品查看所需材料" } },
        { "recipebook_made_at_prefix", { "Made at: ", "制作地点: " } },
        { "back_button", { "Back", "返回" } },
        { "howtoplay_title",  { "How to Play", "游戏玩法" } },
        { "howtoplay_body",
            { "Move with {MOVE} or the arrow keys (hold Shift to sprint).\n"
              "Press {E}, or click a building, to interact with it.\n"
              "Press {U} near a building to instantly upgrade it one level.\n"
              "Press {M} to show/hide the minimap.\n"
              "Walk off the edge of the screen to travel between areas.\n\n"
              "Press {F} next to a built Fishing Dock or Mine/Gold Mine for a quick timing-bar\n"
              "minigame, a Lumber Camp for a mash-to-target one, or an Alchemist/Winery for a\n"
              "memorize-the-sequence one -- all give a bonus catch (short cooldown per activity).\n"
              "Fishing/mining hauls run bigger in Summer (and have a better chance at a rare\n"
              "double haul) and leaner in Winter -- Spring/Autumn stay at the usual rate.\n"
              "In the Highlands, walk up to a berry patch and press {E} to forage a small random\n"
              "bonus (it respawns later). The Traveling Trader in Town Square occasionally has a\n"
              "discounted bundle deal -- talk to them ({E}) to see it.\n\n"
              "PRODUCTION: raw producers (Wheat Farm, Ore Mine, ...) need no input; processors\n"
              "(Bakery, Smelter, ...) consume an input good and stay locked until their\n"
              "prerequisite business is built. Hire up to 5 Workers on one business (from its\n"
              "panel) to boost just that business's output, alongside the Staff Office's global\n"
              "bonus and an optional Foreman Focus on a single business.\n\n"
              "SEASONS: Spring/Summer/Autumn/Winter cycle every 60 in-game days (2 months) each --\n"
              "and each season nudges a different life stat too (Spring: less sickness, Summer:\n"
              "faster energy drain, Autumn: a small boost to every business's output, Winter:\n"
              "faster hunger drain and more sickness). The Farm can be replanted with a different\n"
              "crop from its panel (a small fee applies) -- each crop feeds its own processor and\n"
              "gets a yield bonus during its favorite season, shown in the crop picker. Some\n"
              "villagers will comment on the current season if you talk to them more than once.\n"
              "The Legacy screen's \"Winter Resilience\" option permanently softens Winter's extra\n"
              "penalty, a little more per level, across every future generation.\n\n"
              "MARKET: buy low, sell high -- prices drift over time and react to your own\n"
              "trading and production. Sign a Contract to lock in today's price for later.\n"
              "A few goods also sell for more in their in-demand season: Fish in Summer, Wool\n"
              "in Winter, Pelts in Autumn, Honey in Summer, Milk in Spring.\n\n"
              "LIFE: Sleep restores energy (skips a full day); Eat restores hunger; see the\n"
              "Doctor if you fall sick. Neglecting hunger or sickness for too long is fatal.\n\n"
              "Bank keeps cash safe from theft/recession events (a withdrawal fee applies).\n"
              "Warehouse raises how much of each good you can stockpile.\n\n"
              "Achievements pay a cash reward once unlocked. On death, Legacy points carry over\n"
              "to the next generation for small permanent bonuses -- everything else resets.\n\n"
              "Press Escape any time to pause, save, or open Settings (volume/screen size/key\n"
              "bindings).",

              "使用 {MOVE} 或方向键移动(按住 Shift 可加速)。\n"
              "按 {E} 键,或点击建筑,即可与其互动。\n"
              "在建筑附近按 {U} 键可以直接升级一级。\n"
              "按 {M} 键可以显示/隐藏小地图。\n"
              "走到屏幕边缘可以前往其他区域。\n\n"
              "在建好的渔港、矿场/金矿附近按 {F} 键可以玩计时小游戏,伐木场是狂按目标次数的\n"
              "挑战,炼金坊/酒庄则是记顺序的小游戏——都能拿到额外收获(每种活动各自有短暂\n"
              "冷却时间)。钓鱼/挖矿在夏天收获会更多(而且更容易触发稀有的双倍大丰收),\n"
              "冬天则会少一些——春秋两季维持正常水平。在高地区,走到浆果丛旁按 {E} 可以\n"
              "采集一点随机的小奖励(采过之后过一阵会重新长出来)。市镇广场的行商偶尔会有\n"
              "打折的特惠——按 {E} 跟他聊聊看看。\n\n"
              "生产:原材料产业(麦田农场、矿场等)不需要任何输入;加工产业(面包坊、冶炼厂\n"
              "等)需要消耗一种原料,并且要等它的前置产业建成后才会解锁。可以在产业面板里为\n"
              "单个产业雇佣最多 5 名工人,专门提升这一个产业的产出,这与员工处的全局加成、\n"
              "以及可选的工头专精(只能专精一个产业)是分开叠加的。\n\n"
              "季节:春夏秋冬循环,每季 60 个游戏日(2 个月)。每个季节还会影响不同的生活\n"
              "数值——春天不容易生病、夏天体力消耗更快、秋天全部产业产出小幅提升、冬天\n"
              "饥饿消耗更快且更容易生病。可以在麦田农场的面板里更换作物(需要花一点小钱),\n"
              "不同作物对应不同的加工产业,而且在它适宜的季节产出会更高,选择作物时会标\n"
              "出来。有些村民聊得多了会跟你聊聊当前的季节。传承面板里的\"抗寒传承\"选项可以\n"
              "永久减轻冬季的额外惩罚,等级越高效果越好,对以后每一代角色都有效。\n\n"
              "市场:低买高卖——价格会随时间波动,也会受你自己的交易和生产影响。可以签订\n"
              "合约,把今天的价格锁定到以后再履行。有几种商品在当季需求旺盛时卖价会更高:\n"
              "鱼(夏)、羊毛(冬)、兽皮(秋)、蜂蜜(夏)、牛奶(春)。\n\n"
              "生活:睡觉可以恢复体力(会跳过完整一天);吃饭恢复饥饿度;生病了要去看医生。\n"
              "饥饿或疾病拖太久都会致命。\n\n"
              "钱庄能让现金免受盗窃/经济衰退事件影响(取款需手续费)。仓库可以提高每种商品\n"
              "的库存上限。\n\n"
              "解锁成就会获得现金奖励。角色去世后,传承点数会保留给下一代,用于小幅永久加成\n"
              "——其余一切从头开始。\n\n"
              "随时按 Esc 键可以暂停、存档,或打开设置(音量/画面大小/按键绑定)。" } },
        { "networth_panel_title", { "Net Worth", "净资产" } },

        // Always-visible life status panel (see GameWorld::drawLifeStatusPanel)
        { "status_energy_label", { "Energy", "体力" } },
        { "status_hunger_label", { "Hunger", "饱食度" } },
        { "status_sick_label",   { "Sick: ", "生病: " } },
        { "status_sick_yes",     { "Yes", "是" } },
        { "status_sick_no",      { "No", "否" } },
        { "status_well_rested",  { "Well-rested: +10% production", "精神饱满: 产出 +10%" } },

        // Tutorial
        { "tutorial_title", { "Welcome to Tycoon Idle!", "欢迎来到经营人生!" } },
        { "tutorial_body",  { "You've just turned 21 -- old enough to take over the family business. Good luck!\n\nMove with {MOVE} or the arrow keys (hold Shift to sprint).\nPress {E}, or click a building, to interact with it.\nPress {U} near a building to instantly upgrade it one level.\nPress {M} to show/hide the minimap.\nWalk off the edge of the screen to travel to a new area.\nMost buildings need materials and a few days on-site before they're built -- only the Farm, Lumber Camp, Quarry, and Ore Mine are free and instant.\nBuild the Port, then commission a ship there to sail to Fisher's Isle.\nUnlocking an achievement pops up in the bottom-left corner.\nClick the Achievements button (top-right) any time.\nYour town keeps running in the background, even while you walk around.\nPress Escape any time to pause, save, or open Settings.",
                               "你刚满 21 岁——已经到了可以接手家业的年纪。祝你好运!\n\n使用 {MOVE} 或方向键移动(按住 Shift 可加速)。\n按 {E} 键,或点击建筑,即可与其互动。\n在建筑附近按 {U} 键可以直接升级一级。\n按 {M} 键可以显示/隐藏小地图。\n走到屏幕边缘可以前往新的区域。\n大部分建筑都需要材料,并且要在工地上等上几天才能建成——只有农场、伐木场、采石场、矿场是免费且秒建的。\n建好港口后,在那里造一艘船,就能出海前往渔人岛。\n解锁成就时,左下角会弹出提示。\n随时可以点击右上角的成就按钮。\n你在镇上走动的同时,经济系统也会一直在背后运行。\n随时按 Esc 键可以暂停、存档,或打开设置。" } },
        { "tutorial_continue", { "Press any key or click to begin", "按任意键或点击开始" } },

        // Zone names
        { "zone_town_square", { "Town Square", "市镇广场" } },
        { "zone_farmlands",   { "Farmlands", "农田" } },
        { "zone_mining",      { "Mining District", "矿区" } },
        { "zone_valley",      { "Valley District", "山谷区" } },
        { "zone_harbor",      { "Harbor District", "海港区" } },
        { "zone_highlands",   { "Highlands District", "高地区" } },
        { "zone_market",      { "Market Row", "集市区" } },
        { "zone_fisher_isle", { "Fisher's Isle", "渔人岛" } },

        // Building labels
        { "farm",        { "Farm", "农场" } }, // was "Wheat Farm"/"麦田农场" -- generic now that GameWorld::drawBuilding appends the actually-planted crop (see farmCropId()), since the field can be growing anything from strawberries to cabbage, not just wheat
        { "mine",        { "Ore Mine", "矿场" } },
        { "lumber",      { "Lumber Camp", "伐木场" } },
        { "quarry",      { "Quarry", "采石场" } },
        { "storefront",  { "Storefront", "商铺" } },
        { "storefront_desc", { "Sells straight to walk-in customers -- pays cash directly, no market good involved.",
                                "商铺直接向路过的顾客卖货,收入直接变成现金,不经过市场商品系统。" } },

        // Storefront auto-sell (see Business::autoSellGoodId/autoSellThreshold,
        // Game::trySetStorefrontAutoSell, GameWorld::drawAutoSellOverlay). This
        // is a *separate* feature from storefront_desc's flat cash income above
        // -- the Storefront keeps generating its own passive cash regardless of
        // whether auto-sell is configured, this just adds automatic selling of
        // one chosen warehouse good on top of that.
        { "autosell_configure_button", { "Configure Auto-Sell", "设置自动售卖" } },
        { "autosell_summary_prefix",   { "Auto-sell: ", "自动售卖: " } },
        { "autosell_per_day_suffix",   { "/day", "个/天" } },
        { "autosell_disabled_label",   { "Auto-sell is off.", "自动售卖尚未开启。" } },
        { "autosell_not_built",        { "Build the Storefront first.", "请先建造商铺。" } },
        { "autosell_title",            { "Storefront Auto-Sell", "商铺自动售卖设置" } },
        { "autosell_desc_line1", { "Pick one good below and set a threshold price -- once its market price reaches or exceeds that threshold, the Storefront automatically sells it from your warehouse for you.",
                                    "在下面选一种商品并设定一个门槛价格——一旦该商品的市场价格达到或超过这个门槛,商铺就会自动帮你从仓库里把它卖掉。" } },
        { "autosell_desc_line2", { "The threshold is entirely up to you -- this panel won't tell you whether a price is good. Check the Market panel's current price and trend yourself before deciding.",
                                    "门槛价格完全由你自己判断——这个面板不会替你评估价格好坏,请自己先去市场面板看看当前价格和走势,再决定门槛怎么设。" } },
        { "autosell_desc_line3", { "How much it can sell per in-game day depends on the Storefront's level: 3 at level 1, doubling each level, capping at 48/day from level 5 onward.",
                                    "每天最多能卖出多少取决于商铺等级:1级3个,每升一级翻倍,到5级封顶48个/天(超过5级也维持这个上限)。" } },
        { "autosell_level_prefix",        { "Storefront Lv.", "商铺等级 " } },
        { "autosell_capacity_prefix",     { " -- up to ", " —— 每天最多卖出 " } },
        { "autosell_pick_good_label",     { "Choose a good to auto-sell:", "选择要自动售卖的商品:" } },
        { "autosell_selected_prefix",     { "Selected: ", "已选择: " } },
        { "autosell_current_price_prefix",{ "Current market price: ", "当前市场价: " } },
        { "autosell_threshold_prefix",    { "Sell threshold: ", "售卖门槛: " } },
        { "autosell_status_armed",        { "Price has reached the threshold -- selling now.", "价格已达到门槛,正在自动售卖。" } },
        { "autosell_status_waiting",      { "Waiting for the price to reach the threshold.", "等待价格达到门槛中。" } },
        { "autosell_minus10pct",          { "-10%", "-10%" } },
        { "autosell_minus1pct",           { "-1%", "-1%" } },
        { "autosell_plus1pct",            { "+1%", "+1%" } },
        { "autosell_plus10pct",           { "+10%", "+10%" } },
        { "autosell_set_to_current_button", { "Set Threshold = Current Price", "门槛设为当前价格" } },
        { "autosell_disable_button",      { "Disable Auto-Sell", "禁用自动售卖" } },
        { "bakery",      { "Bakery", "面包坊" } },
        { "smelter",     { "Smelter", "冶炼厂" } },
        { "sawmill",     { "Sawmill", "锯木厂" } },
        { "mason",       { "Mason", "石匠铺" } },
        { "gemshop",     { "Gem Workshop", "宝石工坊" } },
        { "blacksmith",  { "Blacksmith", "铁匠铺" } },
        { "carpenter",   { "Carpenter", "木匠坊" } },
        { "sheep",       { "Sheep Farm", "牧羊场" } },
        { "fishing",     { "Fishing Dock", "渔港" } },
        { "textile",     { "Textile Mill", "纺织厂" } },
        { "tailor",      { "Tailor", "裁缝铺" } },
        { "orchard",     { "Orchard", "果园" } },
        { "herbgarden",  { "Herb Garden", "草药园" } },
        { "goldmine",    { "Gold Mine", "金矿" } },
        { "vineyard",    { "Vineyard", "葡萄园" } },
        { "preserve",    { "Preserve", "果酱坊" } },
        { "apothecary",  { "Apothecary", "药铺" } },
        { "goldsmith",   { "Goldsmith", "金匠铺" } },
        { "winery",      { "Winery", "酒庄" } },
        { "alchemist",   { "Alchemist", "炼金坊" } },
        { "jeweler",     { "Jeweler", "珠宝坊" } },
        { "market",      { "Market", "市场" } },
        { "staff",       { "Staff Office", "职介所" } },
        { "sleep",       { "Bedroom", "卧室" } },
        { "eat",         { "Kitchen", "厨房" } },
        { "doctor",      { "Clinic", "诊所" } },
        { "townhall",    { "Town Hall", "市政厅" } },
        { "bank",        { "Bank", "钱庄" } },
        { "warehouse",   { "Warehouse", "仓库" } },

        // Harbor District
        { "seasalt",      { "Salt Flats", "盐田" } },
        { "pearlfarm",    { "Pearl Farm", "珍珠养殖场" } },
        { "shipyard",     { "Shipyard", "造船坞" } },
        { "pearlatelier", { "Pearl Atelier", "珍珠工坊" } },
        { "port",         { "Port", "港口" } },

        // Fisher's Isle (reached by sailing from the Port -- see zone_fisher_isle)
        { "cannery",      { "Cannery", "罐头厂" } },
        { "smokehouse",   { "Smokehouse", "熏鱼房" } },
        { "deepsea",      { "Deep Sea Fishing", "远洋捕鱼场" } },
        { "sushibar",     { "Sushi Bar", "寿司吧" } },
        { "fishermanplatter", { "Fisherman's Platter", "渔夫拼盘坊" } },
        { "island_ferry", { "Ferry", "渡船" } },

        // Highlands District
        { "dairyfarm", { "Dairy Farm", "奶牛场" } },
        { "beehive",   { "Apiary", "养蜂场" } },
        { "trapper",   { "Trapper's Camp", "猎人营地" } },
        { "teafield",  { "Tea Field", "茶园" } },
        { "flaxfield", { "Flax Field", "亚麻田" } },
        { "creamery",  { "Creamery", "乳品厂" } },
        { "meadery",   { "Meadery", "蜂蜜酒坊" } },
        { "tannery",   { "Tannery", "制革厂" } },
        { "teahouse",  { "Tea House", "茶坊" } },
        { "linenmill", { "Linen Mill", "亚麻纺织厂" } },
        { "giftbasket", { "Country Gift Basket", "田园礼篮坊" } },

        // Farm crop processors
        { "jamkitchen",    { "Jam Kitchen", "果酱厨房" } },
        { "popcornstand",  { "Popcorn Stand", "爆米花摊" } },
        { "juicebar",      { "Juice Bar", "果汁吧" } },
        { "pieshop",       { "Pie Shop", "馅饼铺" } },
        { "roaststand",    { "Roast Stand", "烤薯摊" } },
        { "picklinghouse", { "Pickling House", "腌菜坊" } },

        // Multi-input recipes (see BusinessType::extraInputs) -- Market Row's
        // 3rd row.
        { "honeyrefinery", { "Honey Refinery", "蜜糖坊" } },
        { "cakeshop",      { "Cake Shop", "蛋糕坊" } },
        { "artisanbakery", { "Artisan Bakery", "手工烘焙坊" } },

        // Legend
        { "legend_title",   { "Legend", "图例" } },
        { "legend_tier1",   { "Tier 1: Raw Resources", "一层:原材料" } },
        { "legend_tier2",   { "Tier 2: Processing", "二层:加工" } },
        { "legend_tier3",   { "Tier 3: Advanced Goods", "三层:高级制品" } },
        { "legend_service", { "Services", "生活设施" } },
        { "legend_npc",     { "Villager", "村民" } },

        // NPC names
        { "npc_merchant",    { "Old Merchant", "老商人" } },
        { "npc_mayor",       { "Mayor Wren", "镇长伦恩" } },
        { "npc_farmer",      { "Farmer Nell", "农妇内尔" } },
        { "npc_miner",       { "Miner Gus", "矿工格斯" } },
        { "npc_trader",      { "Traveling Trader", "行商" } },
        { "npc_guard",       { "Town Guard", "镇卫兵" } },
        { "npc_child",       { "Curious Child", "好奇的孩子" } },
        { "npc_prospector",  { "Old Prospector", "老淘金客" } },
        { "npc_orchardist",  { "Orchardist Mabel", "果园主梅布尔" } },
        { "npc_herbalist",   { "Herbalist Fern", "草药师菲恩" } },
        { "npc_prospector2", { "Prospector Jed", "淘金客杰德" } },
        { "npc_vintner",     { "Vintner Rosa", "酿酒师萝莎" } },
        { "npc_shipwright",  { "Shipwright Bram", "造船匠布拉姆" } },
        { "npc_pearldiver",  { "Pearl Diver Nia", "采珠人妮娅" } },
        { "npc_dairymaid",   { "Dairymaid Elsie", "牧场女工艾尔西" } },
        { "npc_beekeeper",   { "Beekeeper Otto", "养蜂人奥托" } },
        { "npc_trapper",     { "Trapper Hollis", "猎人霍利斯" } },
        { "npc_market_vendor", { "Vendor Coraline", "摊主柯拉琳" } },
        { "npc_islander",      { "Islander Kai", "岛民凯" } },

        // Good (market commodity) names
        { "wheat",       { "Wheat", "小麦" } },
        { "ore",         { "Iron Ore", "铁矿" } },
        { "wood",        { "Wood", "木材" } },
        { "stone",       { "Stone", "石头" } },
        { "bread",       { "Bread", "面包" } },
        { "iron_ingot",  { "Iron Ingot", "铁锭" } },
        { "planks",      { "Planks", "木板" } },
        { "bricks",      { "Bricks", "砖块" } },
        { "gem",         { "Gemstone", "宝石" } },
        { "tools",       { "Tools", "工具" } },
        { "furniture",   { "Furniture", "家具" } },
        { "wool",        { "Wool", "羊毛" } },
        { "fish",        { "Fish", "鱼" } },
        { "cloth",       { "Cloth", "布料" } },
        { "smoked_fish", { "Smoked Fish", "熏鱼" } },
        { "clothing",    { "Clothing", "衣物" } },
        { "fruit",       { "Fruit", "水果" } },
        { "preserves",   { "Preserves", "果酱" } },
        { "herbs",       { "Herbs", "草药" } },
        { "medicine",    { "Medicine", "药剂" } },
        { "elixir",      { "Elixir", "灵药" } },
        { "gold_ore",    { "Gold Ore", "金矿石" } },
        { "gold_bars",   { "Gold Bars", "金条" } },
        { "jewelry",     { "Jewelry", "珠宝" } },
        { "grapes",      { "Grapes", "葡萄" } },
        { "wine",        { "Wine", "葡萄酒" } },
        { "salt",          { "Salt", "盐" } },
        { "pearls",        { "Pearls", "珍珠" } },
        { "ships",         { "Ships", "船只" } },
        { "canned_fish",   { "Canned Fish", "罐头鱼" } },
        { "pearl_jewelry", { "Pearl Jewelry", "珍珠饰品" } },
        { "tuna",          { "Tuna", "金枪鱼" } },
        { "sushi",         { "Sushi", "寿司" } },
        { "seafood_platter", { "Seafood Platter", "海鲜拼盘" } },
        { "milk",          { "Milk", "牛奶" } },
        { "cheese",        { "Cheese", "奶酪" } },
        { "honey",         { "Honey", "蜂蜜" } },
        { "mead",          { "Mead", "蜂蜜酒" } },
        { "honey_syrup",   { "Honey Syrup", "蜜糖" } },
        { "cake",          { "Cake", "蛋糕" } },
        { "fruit_bread",   { "Fruit Bread", "果干蜂蜜面包" } },
        { "pelts",         { "Pelts", "兽皮" } },
        { "leather",       { "Leather", "皮革" } },
        { "tea_leaves",    { "Tea Leaves", "茶叶" } },
        { "tea",           { "Tea", "茶" } },
        { "flax",          { "Flax", "亚麻" } },
        { "linen",         { "Linen", "亚麻布" } },
        { "gift_basket",   { "Gift Basket", "礼篮" } },
        { "strawberry",           { "Strawberry", "草莓" } },
        { "corn",                 { "Corn", "玉米" } },
        { "watermelon",           { "Watermelon", "西瓜" } },
        { "pumpkin",              { "Pumpkin", "南瓜" } },
        { "sweetpotato",          { "Sweet Potato", "红薯" } },
        { "cabbage",              { "Cabbage", "卷心菜" } },
        { "strawberry_jam",       { "Strawberry Jam", "草莓酱" } },
        { "popcorn",              { "Popcorn", "爆米花" } },
        { "watermelon_juice",     { "Watermelon Juice", "西瓜汁" } },
        { "pumpkin_pie",          { "Pumpkin Pie", "南瓜派" } },
        { "roasted_sweet_potato", { "Roasted Sweet Potato", "烤红薯" } },
        { "sauerkraut",           { "Sauerkraut", "酸菜" } },

        // Achievement names + descriptions
        { "ach_first_business_name", { "First Steps", "初次尝试" } },
        { "ach_first_business_desc", { "Build your first business.", "建立你的第一个产业。" } },
        { "ach_diversified_name",    { "Diversified Portfolio", "产业大满贯" } },
        { "ach_diversified_desc",    { "Own every kind of business in the production tree.", "拥有产业树中的每一种产业。" } },
        { "ach_thousandaire_name",   { "Thousandaire", "千元户" } },
        { "ach_thousandaire_desc",   { "Reach $1,000 cash.", "现金达到 $1,000。" } },
        { "ach_ten_thousandaire_name", { "Ten Thousandaire", "万元户" } },
        { "ach_ten_thousandaire_desc", { "Reach $10,000 cash.", "现金达到 $10,000。" } },
        { "ach_trader_name",         { "Savvy Trader", "精明商人" } },
        { "ach_trader_desc",         { "Earn $1,000 total from selling goods.", "累计卖货收入达到 $1,000。" } },
        { "ach_tycoon_name",         { "Tycoon", "商业大亨" } },
        { "ach_tycoon_desc",         { "Own 20 combined business levels.", "所有产业等级总和达到 20。" } },
        { "ach_well_staffed_name",   { "Well Staffed", "人手充足" } },
        { "ach_well_staffed_desc",   { "Hire staff up to level 5.", "员工等级达到 5 级。" } },
        { "ach_quarter_century_name", { "Quarter Century", "廿五华诞" } },
        { "ach_quarter_century_desc", { "Reach 25 years old.", "年满 25 岁。" } },
        { "ach_half_century_name",   { "Half Century", "半百之年" } },
        { "ach_half_century_desc",   { "Reach 50 years old.", "年满 50 岁。" } },
        { "ach_centenarian_name",    { "Centenarian", "百岁人瑞" } },
        { "ach_centenarian_desc",    { "Reach the maximum lifespan of 100 years.", "活到 100 岁的寿命上限。" } },
        { "ach_supply_chain_name",   { "Full Supply Chain", "完整供应链" } },
        { "ach_supply_chain_desc",   { "Build all five tier-2 processing businesses.", "建成全部五个二层加工产业。" } },
        { "ach_craftsman_name",      { "Craftsman", "巧匠" } },
        { "ach_craftsman_desc",      { "Reach tier 3 in a production chain (Blacksmith or Carpenter).", "在产业链中达到三层(铁匠铺或木匠坊)。" } },
        { "ach_full_crew_name",      { "Full Crew", "满员配置" } },
        { "ach_full_crew_desc",      { "Fully staff a single business with hired workers.", "把某一个产业的工人雇到满员。" } },
        { "ach_good_timing_name",      { "Good Timing", "当季良机" } },
        { "ach_good_timing_desc",      { "Have the Farm growing a crop during its favorite season.", "让麦田农场在当季种上适宜的作物。" } },
        { "ach_crop_rotator_name",     { "Crop Rotator", "轮作达人" } },
        { "ach_crop_rotator_desc",     { "Change the Farm's crop 3 times in one life.", "本代内更换麦田农场的作物 3 次。" } },
        { "ach_season_cycle_name",     { "Full Cycle", "四季轮回" } },
        { "ach_season_cycle_desc",     { "Live through Spring, Summer, Autumn, and Winter in one life.", "在同一代角色生涯中经历完整的春夏秋冬。" } },
        { "ach_master_farmer_name",    { "Master Farmer", "农事大师" } },
        { "ach_master_farmer_desc",    { "Grow every crop during its favorite season, in one life.", "在同一代角色生涯中,让每种作物都在其适宜季节种植过。" } },

        { "ach_groundbreaking_name",   { "Groundbreaking", "破土动工" } },
        { "ach_groundbreaking_desc",   { "Complete your first construction project.", "完成你的第一个建造项目。" } },
        { "ach_master_builder_name",   { "Master Builder", "建筑大师" } },
        { "ach_master_builder_desc",   { "Have 5 or more constructed businesses standing at once.", "同时拥有5个及以上已建成的产业。" } },
        { "ach_harbormaster_name",     { "Harbormaster", "港务长" } },
        { "ach_harbormaster_desc",     { "Build the Port.", "建成港口。" } },
        { "ach_shipshape_name",        { "Shipshape", "整装待发" } },
        { "ach_shipshape_desc",        { "Commission a ship at the Port.", "在港口建造一艘船。" } },
        { "ach_set_sail_name",         { "Set Sail", "扬帆起航" } },
        { "ach_set_sail_desc",         { "Sail to Fisher's Isle.", "出航前往渔人岛。" } },
        { "ach_island_explorer_name",  { "Island Explorer", "海岛探索者" } },
        { "ach_island_explorer_desc",  { "Own every business on Fisher's Isle.", "拥有渔人岛上的每一个产业。" } },
        { "ach_market_row_regular_name", { "Market Row Regular", "集市常客" } },
        { "ach_market_row_regular_desc", { "Own every stall in Market Row.", "拥有集市区的每一个摊位。" } },
        { "ach_full_stock_name",       { "Full Stock", "货真价实" } },
        { "ach_full_stock_desc",       { "Hold 10 or more different goods in the warehouse at once.", "仓库里同时存有10种及以上不同的商品。" } },
        { "ach_minigame_pro_name",     { "Minigame Pro", "小游戏能手" } },
        { "ach_minigame_pro_desc",     { "Win 5 minigames (fishing, mining, or chopping) in one life.", "本代内在小游戏(钓鱼/挖矿/伐木)中获胜 5 次。" } },
        { "ach_harbor_pioneer_name",   { "Harbor Pioneer", "扬帆起航" } },
        { "ach_harbor_pioneer_desc",   { "Own a business in the Harbor District.", "在海港区拥有一个产业。" } },
        { "ach_highlands_settler_name", { "Highlands Settler", "高原新贵" } },
        { "ach_highlands_settler_desc", { "Own a business in the Highlands District.", "在高地区拥有一个产业。" } },
        { "ach_cat_economy",  { "Economy", "经济" } },
        { "ach_cat_business", { "Business", "产业" } },
        { "ach_cat_life",     { "Life", "人生" } },
        { "ach_cat_season",   { "Seasons", "季节" } },
        { "ach_cat_minigame", { "Minigames", "小游戏" } },
        { "ach_cat_maritime", { "Maritime", "航海" } },

        // Death / generation
        { "death_illness",       { "your untreated illness finally caught up with you", "你未经治疗的疾病最终还是要了你的命" } },
        { "death_starvation",    { "you hadn't eaten in far too long and starved", "你太久没有进食,活活饿死了" } },
        { "death_generic",       { "your story came to an end", "你的故事到此结束" } },
        { "death_age_prefix",    { "At ", "在 " } },
        { "death_age_suffix",    { " years old, ", " 岁那年," } },
        { "death_final_estate",  { "Final estate: $", "最终身家:$" } },
        { "death_staff_level",   { ", staff level ", ",员工等级 " } },
        { "death_peak_prefix",         { "Peak cash this life: $", "本代最高身家: $" } },
        { "death_achievements_prefix", { ". Achievements unlocked: ", "。已解锁成就: " } },
        { "death_achievements_suffix", { ".", "。" } },
        { "death_family_inherits", { "Your family inherits $", "你的家族继承了 $" } },
        { "death_foothold_mid",  { " and a foothold in the ", ",并在" } },
        { "death_foothold_suffix", { " trade (starting at level 1).", "行业中占有一席之地(从 1 级开始)。" } },
        { "generation_prefix",   { "Generation ", "第 " } },
        { "generation_suffix",   { " begins. Your achievements carry over; everything else starts fresh.", " 代传人开始了。你的成就得以保留,其余一切从头开始。" } },

        // Achievement unlock announcement
        { "achievement_prefix", { "[Achievement] ", "[成就] " } },
        { "achievement_toast_header", { "Achievement Unlocked!", "成就达成!" } },
        { "returned_to_harbor", { "Back at the Harbor District.", "已返回海港区。" } },
        { "reward_label",       { "reward $", "奖励 $" } },

        // Event log overflow
        { "more_events_prefix", { "...and ", "...还有 " } },
        { "more_events_suffix", { " more events happened.", " 个事件发生了。" } },

        // Status screen
        { "status_title_prefix",     { "Tycoon Idle (Generation ", "经营人生(第 " } },
        { "status_title_suffix",     { ")", " 代)" } },
        { "status_cash",             { "Cash: $", "现金: $" } },
        { "status_staff_lv",         { "   Staff Lv.", "   员工等级 " } },
        { "status_staff_mult_prefix", { " (x", " (x" } },
        { "status_staff_mult_suffix", { " production; rates below are base, before this bonus)\n", " 倍产出;下方数值为未加成的基础值)\n" } },
        { "status_upkeep_prefix",     { "Upkeep: -$", "维护费: -$" } },
        { "status_upkeep_suffix",     { "/sec (total across all business levels)\n", "/秒(所有产业等级总计)\n" } },
        { "status_age",              { "Age: ", "年龄: " } },
        { "status_yrs",               { " yrs", " 岁" } },
        { "status_age_eff_prefix",   { " (age efficiency x", " (年龄效率 x" } },
        { "status_age_eff_suffix",   { ")", ")" } },
        { "status_energy",           { "   Energy: ", "   体力: " } },
        { "status_hunger",           { "   Hunger: ", "   饥饿度: " } },
        { "status_prod_penalty_prefix", { "   [production x", "   [产出 x" } },
        { "status_prod_penalty_suffix", { " -- sleep/eat/see a doctor to recover]", " -- 睡觉/吃饭/看医生可恢复]" } },
        { "status_sick_prefix",      { "  [SICK -- see a doctor! (", "  [生病了——快去看医生!(" } },
        { "status_sick_suffix",      { " days untreated, fatal if it reaches the max)]\n", " 天未治疗,达到上限会致命)]\n" } },
        { "status_starving_prefix",  { "  [STARVING (", "  [饥饿中(" } },
        { "status_starving_suffix",  { " days, fatal if it reaches the max) -- eat!]\n", " 天,达到上限会致命)——快吃东西!]\n" } },
        { "section_businesses",      { "------------------------- Businesses ----------------", "------------------------- 产业列表 ----------------" } },
        { "section_market",          { "------------------------- Market --------------------", "------------------------- 市场行情 --------------------" } },

        // Manage Businesses menu
        { "menu_businesses_header",  { "\n-- Manage Businesses --\n", "\n-- 产业管理 --\n" } },
        { "menu_businesses_prompt",  { "Enter a business # to build/upgrade one level (0 to go back): ", "输入产业编号来建造/升级一级(输入 0 返回): " } },
        { "invalid_business_number", { "Invalid business number.\n", "无效的产业编号。\n" } },
        { "business_action_prompt",  { "1) Upgrade level   2) Hire worker   0) Cancel\nChoice: ", "1) 升级等级   2) 雇佣工人   0) 取消\n选择: " } },
        { "worker_hired_prefix",     { "Hired a worker for ", "已为" } },
        { "worker_hired_mid",        { " -- now ", "雇佣了一名工人——现在 " } },
        { "worker_hired_suffix",     { " workers, cost $", " 名工人,花费 $" } },
        { "worker_needs_level",      { "Build this business first.\n", "请先建造该产业。\n" } },
        { "workers_maxed",           { "Already at the max number of workers.\n", "工人数量已达上限。\n" } },
        { "workers_label",           { "Workers: ", "工人: " } },
        { "hire_worker_button",      { "Hire Worker", "雇佣工人" } },
        { "farm_action_prompt",      { "1) Upgrade level   2) Hire worker   3) Change crop   0) Cancel\nChoice: ", "1) 升级等级   2) 雇佣工人   3) 更换作物   0) 取消\n选择: " } },
        { "crop_picker_header",      { "\n-- Choose a Crop --\n", "\n-- 选择作物 --\n" } },
        { "crop_switch_cost_prefix", { "Switching crops costs $", "切换作物需要花费 $" } },
        { "crop_choice_prompt",      { "Choice: ", "选择: " } },
        { "crop_changed_prefix",     { "Now growing ", "现在种植的是 " } },
        { "crop_favorite_prefix",    { " (favors ", " (适宜" } },
        { "crop_favorite_suffix",    { ")", ")" } },
        { "current_crop_label",      { "Current crop: ", "当前作物: " } },
        { "change_crop_button",      { "Change Crop", "更换作物" } },
        { "season_bonus_active",     { " [in season!]", " [当季加成!]" } },
        { "invalid_crop",            { "Invalid crop.\n", "无效的作物。\n" } },
        { "crop_needs_farm",         { "Build the Farm first.\n", "请先建造麦田农场。\n" } },
        { "crop_already_active",     { "Already growing that crop.\n", "已经在种这个作物了。\n" } },
        { "fishing_needs_dock",      { "Build the Fishing Dock first.", "请先建造渔港。" } },
        { "minigame_needs_building", { "Build that first.", "请先建造它。" } },
        { "fishing_title",           { "Fishing", "钓鱼" } },
        { "fishing_hint",            { "Press Space (or click Catch!) when the marker is in the green zone.", "指针进入绿色区域时按空格键(或点击\"捕捞!\")。" } },
        { "fishing_catch_button",    { "Catch!", "捕捞!" } },
        { "fishing_hit_prefix",      { "Great catch! +", "捕获成功!+" } },
        { "fishing_miss_prefix",     { "Just missed. +", "差一点没捕到。+" } },
        { "minigame_rare_prefix",    { "Rare haul! +", "稀有大丰收!+" } },
        { "fishing_result_suffix",   { " fish.", " 条鱼。" } },
        { "minigame_result_suffix", { ".", "。" } },
        { "mining_title",           { "Mining", "挖矿" } },
        { "mining_catch_button",    { "Mine!", "挖掘!" } },
        { "chopping_title",          { "Chopping", "伐木" } },
        { "chopping_hint",           { "Mash Space (or click Chop!) to reach the target before time runs out.", "在时间用完之前狂按空格键(或点击\"砍伐!\")达到目标次数。" } },
        { "chopping_progress_prefix", { "Chops: ", "砍伐次数: " } },
        { "chopping_button",         { "Chop!", "砍伐!" } },
        { "forage_prefix",           { "Foraged +", "采集到了 +" } },
        { "brewing_title",          { "Brewing", "调配" } },
        { "brewing_hint_memorize",  { "Memorize the sequence...", "记住这个顺序……" } },
        { "brewing_hint_repeat",    { "Now click the colors in the same order.", "现在按相同顺序点击颜色。" } },
        { "farmer_season_line_prefix",  { "This season is ", "现在是" } },
        { "farmer_season_line_mid",     { ". The Farm's growing ", "。农场现在种的是" } },
        { "farmer_season_line_in_season", { " -- perfect timing for it!", "——正是好时候!" } },
        { "farmer_season_line_off_prefix", { ", though it really shines in ", ",不过它在" } },
        { "farmer_season_line_off_suffix", { ".", "长得更好。" } },

        // Other NPCs' every-other-visit seasonal remarks (see kSeasonalCommentNpcs) --
        // simpler than Farmer Nell's crop-aware line, just one flavor sentence per season.
        { "npc_orchardist_season_spring", { "The blossoms are out early this year -- a good sign for the harvest.", "今年花开得早,看来今年的果子会丰收。" } },
        { "npc_orchardist_season_summer", { "The fruit's swelling nicely in this heat, just needs a bit more rain.", "这大热天倒是让果子长得挺快,就是缺点雨水。" } },
        { "npc_orchardist_season_autumn", { "Harvest time! Come see the orchard groaning with fruit.", "收获的季节到了!快来看看果园里压弯枝头的果子。" } },
        { "npc_orchardist_season_winter", { "The trees are bare now -- just pruning and waiting until spring.", "树都光秃秃的了,现在就是修枝,等着春天到来。" } },
        { "npc_beekeeper_season_spring",  { "The bees are finally waking up -- busy little things, aren't they?", "蜜蜂总算醒过来了,一个个忙得很呢。" } },
        { "npc_beekeeper_season_summer",  { "Peak honey season! The hives are working overtime.", "现在是产蜜旺季!蜂巢里忙得热火朝天。" } },
        { "npc_beekeeper_season_autumn",  { "Slowing the hives down for winter -- can't let them overwork now.", "得让蜂群慢慢过冬了,不能再让它们太辛苦。" } },
        { "npc_beekeeper_season_winter",  { "The hives are quiet this time of year. Just keeping them warm and waiting.", "这个季节蜂巢都很安静,我就负责保暖,等着春天。" } },
        { "npc_trapper_season_spring",    { "Game's coming out of hiding now that the cold's broken.", "天暖和了,猎物们都从藏身处出来了。" } },
        { "npc_trapper_season_summer",    { "Tracks are harder to find in the summer growth, but the pelts aren't worth much now anyway.", "夏天草长得密,踪迹难找,不过这时候的皮毛也不值钱。" } },
        { "npc_trapper_season_autumn",    { "Best pelts of the year come in autumn -- thick and ready for winter.", "秋天的皮毛最厚实,正好准备过冬用。" } },
        { "npc_trapper_season_winter",    { "Cold enough that the animals barely move. Patience is the name of the game.", "冷成这样,动物都懒得动了,得有耐心才行。" } },
        { "npc_mayor_season_spring",      { "Spring's a good time for the town -- everyone's in better spirits.", "春天对小镇来说是好时节,大家精神头都好多了。" } },
        { "npc_mayor_season_summer",      { "Keep hydrated out there -- the summer heat can wear on anyone.", "外面挺热的,记得多喝水,夏天很容易累坏身子。" } },
        { "npc_mayor_season_autumn",      { "Harvest season keeps the whole town busy. Good for business, I'd say.", "丰收的季节让全镇都忙起来了,对生意也是好事。" } },
        { "npc_mayor_season_winter",      { "Stay warm this winter -- and don't forget to eat well and see the doctor if you're unwell.", "冬天要注意保暖——记得按时吃饭,身体不舒服就去看医生。" } },
        { "deal_intro_prefix",  { "I've got a deal: ", "我这有个好买卖:" } },
        { "deal_price_prefix",  { " for just $", ",只要 $" } },
        { "deal_price_suffix",  { "!", "!" } },
        { "deal_buy_button",    { "Buy Deal", "买下" } },
        { "deal_thanks",        { "Pleasure doing business!", "跟你做生意真愉快!" } },
        { "deal_bought_prefix", { "Bought the deal! -$", "买下了!花费 $" } },
        { "fishing_cooldown_prefix", { "The fish need a break -- try again in ", "鱼儿需要喘口气——请在 " } },
        { "fishing_cooldown_suffix", { "s.", " 秒后再试。" } },
        { "locked_prefix",           { "Locked -- build a ", "尚未解锁——需要先建" } },
        { "locked_suffix",           { " first (see the Production Tree).\n", "(可查看产业树)。\n" } },
        { "not_enough_cash_prefix",  { "Not enough cash. Need $", "现金不足,还需要 $" } },
        { "upgraded_prefix",         { "Upgraded ", "已升级 " } },
        { "upgraded_mid",            { " to level ", " 到 " } },
        { "upgraded_suffix",         { " for $", " 级,花费 $" } },
        { "bulk_upgrade_prompt",     { "Upgrade how many levels? (0 = as many as you can afford): ", "要升级多少级?(0 = 尽可能多升级): " } },
        { "bulk_upgraded_mid",       { " by ", " " } },
        { "bulk_upgraded_levels_suffix", { " levels (now level ", " 级(现在等级 " } },
        { "bulk_upgraded_cost_suffix",   { ") for $", ")花费 $" } },

        // Market menu
        { "menu_market_header",   { "\n-- Trade at Market --\n", "\n-- 市场交易 --\n" } },
        { "menu_market_action",   { "1) Buy   2) Sell   3) Contracts   0) Back\nChoice: ", "1) 买入   2) 卖出   3) 合约   0) 返回\n选择: " } },
        { "invalid_choice",       { "Invalid choice.\n", "无效选择。\n" } },
        { "enter_good_number",    { "Enter good # : ", "输入商品编号: " } },
        { "invalid_good_number",  { "Invalid good number.\n", "无效的商品编号。\n" } },
        { "enter_quantity",       { "Enter quantity: ", "输入数量: " } },
        { "invalid_quantity",     { "Invalid quantity.\n", "无效数量。\n" } },
        { "cant_afford_prefix",   { "Can't afford that. Cost would be $", "买不起,需要 $" } },
        { "bought_prefix",        { "Bought ", "已购买 " } },
        { "bought_mid",           { " for $", " 花费 $" } },
        { "dont_have_that_much_prefix", { "You don't have that much ", "你没有那么多" } },
        { "dont_have_that_much_suffix", { " to sell.\n", "可以卖。\n" } },
        { "sold_prefix",          { "Sold ", "已卖出 " } },
        { "sold_mid",             { " for $", " 获得 $" } },

        // Contracts submenu -- lock today's price for a good, cash in later regardless of market moves
        { "menu_contracts_header",  { "\n-- Contracts --\n", "\n-- 合约 --\n" } },
        { "contract_sign_option",   { "Sign a new contract", "签订新合约" } },
        { "contract_choice_prompt", { "Choice: ", "选择: " } },
        { "contract_fulfilled_prefix", { "Contract fulfilled: +$", "合约已履行:+$" } },
        { "contract_signed_prefix",    { "Signed a contract locking ", "已签订合约,锁定" } },
        { "contract_signed_mid",       { " at $", " 价格为 $" } },
        { "contract_slots_full",   { "You already have the maximum number of contracts signed.\n", "你已经签了最多数量的合约了。\n" } },
        { "contract_no_stock",     { "You have none of that good in stock to fulfill the contract with.\n", "你目前没有该商品库存,无法履行合约。\n" } },
        { "contracts_button",      { "Contracts", "合约" } },
        { "fulfill_button",        { "Fulfill", "履行" } },
        { "contract_none_active",  { "No contracts signed yet.", "还没有签订任何合约。" } },
        { "contract_sign_selected_prefix", { "Lock in ", "锁定" } },

        // Staff menu
        { "menu_staff_header",       { "\n-- Staff --\n", "\n-- 员工管理 --\n" } },
        { "staff_current_prefix",    { "Current staff level: ", "当前员工等级: " } },
        { "staff_current_suffix",    { " (production x", " (产出 x" } },
        { "staff_cost_prefix",       { "Cost to hire next level: $", "雇佣下一级花费: $" } },
        { "staff_hire_prompt",       { "1) Hire   2) Set foreman focus   0) Back\nChoice: ", "1) 雇佣   2) 设置工头专精   0) 返回\n选择: " } },
        { "staff_hired_prefix",      { "Hired! Staff level is now ", "雇佣成功!员工等级现在是 " } },
        { "staff_hired_suffix",      { ", all production x", ",全部产出 x" } },
        { "staff_focus_label",       { "Foreman focus: ", "工头专精: " } },
        { "staff_focus_none",        { "none", "无" } },
        { "staff_focus_suffix",      { " (extra +", "(额外 +" } },
        { "staff_focus_pick_header", { "\nPick a business to focus on:\n", "\n选择一个要专精的产业:\n" } },
        { "staff_focus_clear_option", { "0) Clear focus\n", "0) 取消专精\n" } },
        { "staff_focus_cleared",     { "Foreman focus cleared.\n", "已取消工头专精。\n" } },
        { "staff_focus_set_prefix",  { "Foreman now focused on ", "工头现在专精于 " } },
        { "staff_focus_clarify",     { "Stacks with that business's own hired workers -- a separate bonus, not a duplicate.",
                                        "这是独立于该产业自己雇佣的员工的额外加成,两者可以同时生效,并非重复功能。" } },

        // Sleep menu
        { "menu_sleep_header",   { "\n-- Sleep --\n", "\n-- 睡觉 --\n" } },
        { "sleep_desc_prefix",   { "Sleeping skips ahead one full in-game day (", "睡觉会跳过完整的一个游戏日(现实中约 " } },
        { "sleep_desc_suffix",   { " of real time) and fully restores your energy. Hunger keeps ticking down as normal.\n", "),并完全恢复体力。饥饿度仍会照常下降。\n" } },
        { "sleep_warning_fatal", { "WARNING: at this rate, sleeping now will starve you badly enough to be fatal. Eat something first!\n", "警告: 按目前的速度,现在睡觉会饿到足以致命。先吃点东西吧!\n" } },
        { "sleep_warning_hunger", { "Note: your hunger will hit 0 partway through the night. You'll wake up starving.\n", "注意: 睡到一半你的饥饿度就会降到 0,醒来时会处于饥饿状态。\n" } },
        { "sleep_prompt",        { "Go to sleep? (1 = yes, 0 = back): ", "要睡觉吗?(1 = 是, 0 = 返回): " } },
        { "sleep_prompt2",       { "1) Sleep  2) Upgrade Bedroom  0) Back: ", "1) 睡觉  2) 升级卧室  0) 返回: " } },
        { "sleep_died",          { "You fall asleep... and don't wake up.\n", "你睡着了……却再也没有醒来。\n" } },
        { "sleep_woke",          { "You wake up refreshed. Energy restored to 100.\n", "你醒来时精神饱满,体力恢复到 100。\n" } },
        { "sleep_well_rested",   { "You feel well-rested! Production is up for the next few hours.\n", "你感觉精神饱满!接下来几小时产出会提升。\n" } },

        // Bedroom upgrade (reached from the Sleep menu -- see Game::bedroomLevel/tryUpgradeBedroom)
        { "bedroom_level_prefix",        { "Bedroom: Lv ", "卧室: 等级 " } },
        { "bedroom_effect_prefix",       { " -- sleeping grants a ", " —— 睡觉可获得 " } },
        { "bedroom_effect_mid",          { "-hour well-rested buff (+", " 小时的精神饱满加成(产出 +" } },
        { "bedroom_effect_suffix",       { "% production)\n", "%)\n" } },
        { "bedroom_upgrade_cost_prefix", { "Upgrade cost: $", "升级花费: $" } },
        { "bedroom_upgrade_cost_suffix", { "\n", "\n" } },
        { "bedroom_upgraded_prefix",     { "Bedroom upgraded to level ", "卧室已升级到 " } },
        { "bedroom_maxed",               { "Bedroom is already at its maximum level.\n", "卧室已经是最高等级了。\n" } },

        // Eat menu -- wheat used to be the only edible good (see
        // Game.cpp's kFoodDefs for the full table it grew into), so these
        // used to just say "Wheat" outright; now the food's own name is
        // inserted between eat_have_prefix/eat_have_mid and between
        // ate_prefix/ate_suffix instead.
        { "menu_eat_header",     { "\n-- Eat --\n", "\n-- 吃饭 --\n" } },
        { "no_food_source",      { "No food available -- you have none of anything edible right now.\n", "现在没有任何可以吃的东西。\n" } },
        { "hunger_label",        { "Hunger: ", "饥饿度: " } },
        { "eat_have_prefix",     { "You have ", "你有 " } },
        { "eat_have_mid",        { " (each unit restores ", " (每单位恢复 " } },
        { "eat_have_suffix",     { " hunger)\n", " 点饥饿度)\n" } },
        { "eat_pick_prompt",     { "Pick a food number (0 to go back): ", "选择食物编号(0 返回): " } },
        { "eat_prompt",          { "Eat how many units? (0 to go back): ", "要吃多少单位?(0 返回): " } },
        { "dont_have_that_food", { "You don't have that much of that.\n", "你没有那么多这种食物。\n" } },
        { "not_hungry",          { "You're already full -- no need to eat right now.\n", "你已经吃饱了,现在不需要再吃。\n" } },
        { "ate_prefix",          { "Ate ", "已食用 " } },
        { "ate_suffix",          { ". Hunger is now ", "。饥饿度现在是 " } },
        { "ate_variety_bonus",   { " (+15% varied diet bonus!)\n", "(+15% 饮食多样化加成!)\n" } },
        { "eat_selected_prefix", { "Selected: ", "已选择: " } },

        // Doctor menu
        { "menu_doctor_header",     { "\n-- See a Doctor --\n", "\n-- 看医生 --\n" } },
        { "not_sick",               { "You're not sick. Nothing to treat.\n", "你没有生病,没什么可以治疗的。\n" } },
        { "doctor_desc_line1", { "Getting sick is random and unrelated to hunger/energy -- it can happen on any given day, though Winter makes it noticeably more likely and Spring noticeably less.",
                                   "生病是随机事件,跟饥饿度/体力没有直接关系——任何一天都可能发生,不过冬天概率明显更高、春天明显更低。" } },
        { "doctor_desc_line2", { "While sick, every business's output is stuck at 70% until you're treated -- and if it drags on long enough without treatment, it's fatal.",
                                   "生病期间,所有产业的产出都会被锁在70%,直到治好为止——而且拖得太久不治疗是会死人的。" } },
        { "doctor_desc_line3", { "Come back here and pay to be treated the moment you notice you're sick (see the status panel in the top-right of the screen) -- there's no reason to wait it out.",
                                   "一发现自己生病了(看屏幕右上角的状态面板)就该回来这里花钱治疗——没有理由硬扛。" } },
        { "sick_for_prefix",        { "You've been sick for ", "你已经病了 " } },
        { "sick_for_suffix",        { " in-game days (fatal at ", " 个游戏日(达到 " } },
        { "sick_penalty_note",      { "All business output is at 70% while sick.", "生病期间所有产业产出都只有正常的70%。" } },
        { "sick_for_suffix2",       { " days untreated).\n", " 天未治疗会致命)。\n" } },
        { "treatment_cost_prefix",  { "Treatment costs $", "治疗费用 $" } },
        { "treatment_cost_suffix",  { ". Get treated? (1 = yes, 0 = back): ", "。要接受治疗吗?(1 = 是, 0 = 返回): " } },
        { "all_better",             { "All better! Sickness cured.\n", "痊愈了!疾病已治好。\n" } },

        // Legacy menu (prestige points, earned on death, spent on permanent bonuses)
        { "menu_legacy_header",        { "\n-- Legacy --\n", "\n-- 传承 --\n" } },
        { "legacy_points_label",       { "Legacy Points: ", "传承点数: " } },
        { "legacy_cash_option_prefix", { "Starting Cash Bonus: +$", "初始现金加成: +$" } },
        { "legacy_cash_option_mid",    { " (next level costs ", "(下一级需要 " } },
        { "legacy_prod_option_prefix", { "Production Bonus: +", "产出加成: +" } },
        { "legacy_prod_option_mid",    { "% (next level costs ", "%(下一级需要 " } },
        { "legacy_season_option_prefix", { "Winter Resilience: -", "抗寒传承: -" } },
        { "legacy_season_option_mid",    { "% of Winter's extra hunger/sickness penalty (next level costs ", "%冬季额外的饥饿/生病惩罚(下一级需要 " } },
        { "legacy_season_maxed_suffix",  { "-- maxed out, Winter's extra penalty is fully cancelled)", "——已满级,冬季额外惩罚已完全抵消)" } },
        { "legacy_season_maxed",         { "Already at the maximum level.\n", "已经是最高等级了。\n" } },
        { "legacy_points_suffix",      { " points)", " 点)" } },
        { "legacy_back_option",        { "0) Back", "0) 返回" } },
        { "legacy_choice_prompt",      { "Choice: ", "选择: " } },
        { "legacy_bought_prefix",      { "Upgraded! Now level ", "升级成功!现在等级 " } },
        { "legacy_not_enough_points",  { "Not enough legacy points.\n", "传承点数不足。\n" } },
        { "legacy_earned_prefix",      { "Earned ", "获得了 " } },
        { "legacy_earned_suffix",      { " legacy points (total: ", " 点传承点数(总计: " } },

        // Generation history / leaderboard (shown on the Legacy screen)
        { "history_header",       { "\nPast Generations:\n", "\n历代记录:\n" } },
        { "history_entry_prefix", { "Gen ", "第 " } },
        { "history_entry_mid1",   { ": peak $", " 代:最高 $" } },
        { "history_entry_mid2",   { ", lived ", ",活了 " } },
        { "history_entry_mid3",   { " yrs -- ", " 岁——" } },

        // Bank menu -- deposits are safe from theft/recession events, at the cost of a withdrawal fee
        { "menu_bank_header",     { "\n-- Bank --\n", "\n-- 钱庄 --\n" } },
        { "bank_cash_label",      { "Cash on hand: $", "手头现金: $" } },
        { "bank_balance_label",   { "In the bank: $", "存在钱庄: $" } },
        { "bank_fee_note",        { " (withdrawal fee: ", "(取款手续费: " } },
        { "bank_action_prompt",   { "1) Deposit   2) Withdraw   0) Back\nChoice: ", "1) 存款   2) 取款   0) 返回\n选择: " } },
        { "bank_amount_prompt",   { "Amount: ", "金额: " } },
        { "bank_deposited_prefix", { "Deposited $", "已存入 $" } },
        { "bank_withdrew_prefix", { "Withdrew $", "已取出 $" } },
        { "bank_invalid_amount",  { "Invalid amount for that account.\n", "金额无效。\n" } },

        // Warehouse menu -- raises the per-good stock cap
        { "menu_warehouse_header",   { "\n-- Warehouse --\n", "\n-- 仓库 --\n" } },
        { "warehouse_level_label",   { "Warehouse level: ", "仓库等级: " } },
        { "warehouse_cap_label",     { "   (max per good: ", "   (每种商品上限: " } },
        { "warehouse_inventory_header", { "Current Inventory", "现有库存" } },
        { "warehouse_empty_hint",    { "The warehouse is empty so far.", "仓库目前还是空的。" } },
        { "warehouse_cost_prefix",   { "Upgrade cost: $", "升级花费: $" } },
        { "warehouse_upgrade_prompt", { "Upgrade? (1 = yes, 0 = back): ", "要升级吗?(1 = 是, 0 = 返回): " } },
        { "warehouse_upgraded_prefix", { "Warehouse upgraded to level ", "仓库已升级到 " } },
        { "warehouse_full",          { "Warehouse is full for that good.\n", "该商品的仓库已经满了。\n" } },

        // Fast-forward menu
        { "menu_fastforward_header", { "\n-- Fast-forward (simulate being AFK) --\n", "\n-- 快进(模拟挂机)--\n" } },
        { "minutes_to_simulate",     { "Minutes to simulate: ", "要模拟多少分钟: " } },
        { "invalid_amount",          { "Invalid amount.\n", "无效数值。\n" } },
        { "simulated_prefix",        { "Simulated ", "已模拟 " } },
        { "simulated_suffix",        { " of AFK time.\n", " 的挂机时间。\n" } },
        { "cash_earned_prefix",      { "Cash earned: $", "获得现金: $" } },

        // Achievements menu
        { "menu_achievements_header", { "\n-- Achievements --\n", "\n-- 成就 --\n" } },
        { "achievements_progress_prefix", { "Unlocked: ", "已解锁: " } },

        // Main in-game menu
        { "main_menu_1",  { "1) Manage Businesses", "1) 产业管理" } },
        { "main_menu_2",  { "2) View Production Tree", "2) 查看产业树" } },
        { "main_menu_3",  { "3) Trade at Market", "3) 市场交易" } },
        { "main_menu_4",  { "4) Hire Staff", "4) 雇佣员工" } },
        { "main_menu_5",  { "5) Sleep", "5) 睡觉" } },
        { "main_menu_6",  { "6) Eat", "6) 吃饭" } },
        { "main_menu_7",  { "7) See Doctor", "7) 看医生" } },
        { "main_menu_8",  { "8) Fast-forward (simulate AFK time)", "8) 快进(模拟挂机)" } },
        { "main_menu_9",  { "9) Achievements", "9) 成就" } },
        { "main_menu_legacy", { "10) Legacy", "10) 传承" } },
        { "main_menu_bank",      { "11) Bank", "11) 钱庄" } },
        { "main_menu_warehouse", { "12) Warehouse", "12) 仓库" } },
        { "main_menu_10", { "13) Save", "13) 保存" } },
        { "main_menu_11", { "14) Save & Quit", "14) 保存并退出" } },

        // Load/save / welcome back / top-level loop
        { "welcome_back_prefix",  { "Welcome back! You were away for ", "欢迎回来!你离开了 " } },
        { "welcome_back_suffix",  { ".\n", "。\n" } },
        { "idle_earnings_prefix", { "Idle earnings: $", "挂机收入: $" } },
        // Offline safety net (see Game::kOfflineSafetyMarginDays/WelcomeBackInfo::
        // nearFatalWhileAway) -- shown both to the console and the in-window
        // Welcome Back overlay when neglect while away almost killed the character.
        { "welcome_back_near_fatal", { "You barely survived while you were away -- eat and/or see a doctor RIGHT NOW.\n", "你离开期间差点没能撑过来——现在立刻去吃东西和/或看医生。\n" } },

        // In-window Welcome Back overlay (see GameWorld::drawWelcomeBackOverlay) --
        // the graphical counterpart to the console welcome_back_prefix lines
        // above, which are invisible once the SFML window covers the console.
        { "welcomeback_title",        { "Welcome Back!", "欢迎回来!" } },
        { "welcomeback_hint",         { "Click to see what happened while you were away", "点击查看你离开期间发生了什么" } },
        { "welcomeback_away_prefix",  { "You were away for ", "你离开了 " } },
        { "welcomeback_nothing_happened", { "Nothing eventful happened.", "没有发生什么特别的事。" } },
        { "saved",                { "Saved.\n", "已保存。\n" } },
        { "input_closed",         { "\nInput closed, saving and exiting.\n", "\n输入已关闭,正在保存并退出。\n" } },
        { "please_enter_number",  { "Please enter a number.\n", "请输入一个数字。\n" } },
        { "invalid_choice_menu",  { "Invalid choice.\n", "无效选择。\n" } },
        { "final_farewell",       { "\nSaved. Your empire keeps earning while you're away -- see you next time!\n", "\n已保存。你的产业会在你离开时继续运作——下次再见!\n" } },

        // Business table columns
        { "col_hash",     { "#", "#" } },
        { "col_business", { "Business", "产业" } },
        { "col_level",    { "Level", "等级" } },
        { "col_rate",     { "Rate/sec", "速率/秒" } },
        { "col_good",     { "Good", "产出" } },
        { "col_needs",    { "Needs", "所需" } },
        { "col_cost",     { "Upgrade Cost", "升级花费" } },
        { "col_price",    { "Price", "价格" } },
        { "col_hold",     { "You Hold", "持有量" } },
        { "locked_label", { "locked", "未解锁" } },
        { "not_built_label", { "not built", "未建造" } },
        { "cash_label",   { "$ (cash)", "$ (现金)" } },
        { "tree_header",  { "Production Tree (indented items unlock once their parent is built)", "产业树(缩进项目会在其上级建成后解锁)" } },
        { "scroll_hint",  { "Scroll with the mouse wheel", "用鼠标滚轮滚动查看" } },

        // Market overlay filter/sort bar (see drawMarketOverlay/MarketFilter/MarketSort)
        { "market_filter_all",       { "All", "全部" } },
        { "market_filter_raw",       { "Raw Materials", "原材料" } },
        { "market_filter_processed", { "Processed", "加工品" } },
        { "market_filter_owned",     { "In Stock", "已持有" } },
        { "market_sort_prefix",      { "Sort: ", "排序: " } },
        { "market_sort_default",     { "Default", "默认" } },
        { "market_sort_name",        { "Name A-Z", "名称 A-Z" } },
        { "market_sort_price_desc",  { "Price High-Low", "价格 高-低" } },
        { "market_sort_price_asc",   { "Price Low-High", "价格 低-高" } },
        { "market_sort_stock_desc",  { "Quantity High-Low", "数量 高-低" } },
        { "market_sort_stock_asc",   { "Quantity Low-High", "数量 低-高" } },
        { "market_filter_empty_hint", { "No goods match this filter.", "没有符合此筛选条件的商品。" } },
        { "rate_note",    { "* base rate per level -- actual output also includes Staff/Worker/Season bonuses",
                             "* 未加成的基础值——实际产出还会包含员工处/雇工/季节加成" } },
        { "tree_level_prefix", { "Lv.", "等级" } },

        // Events
        { "event_prefix",         { "[Event] ", "[事件] " } },
        { "event_surge_suffix",   { " prices surged! Now $", " 价格暴涨!现在是 $" } },
        { "event_crash_suffix",   { " prices crashed! Now $", " 价格暴跌!现在是 $" } },
        { "event_windfall_prefix", { "Found a lucky windfall: +$", "捡到一笔意外之财:+$" } },
        { "event_theft_prefix",   { "A break-in cost you $", "遭遇入室盗窃,损失了 $" } },
        { "event_spoilage_prefix", { "Some ", "部分" } },
        { "event_spoilage_mid",   { " spoiled in storage: -", " 在仓库里坏掉了:-" } },
        { "event_spoilage_suffix", { " units.", " 个单位。" } },
        { "event_illness",        { "You've fallen ill. See a doctor before it gets worse.", "你生病了,趁还没恶化赶紧去看医生吧。" } },
        { "event_market_crash",   { "Market-wide crash! Every good's price just tumbled.", "市场全面暴跌!所有商品价格集体下挫。" } },
        { "event_market_boom",    { "Economic boom! Every good's price just jumped.", "经济繁荣!所有商品价格集体飙升。" } },
        { "event_disaster_prefix", { "Disaster struck your ", "遭遇天灾,你的" } },
        { "event_disaster_mid",   { " stockpile: lost ", " 库存损失了 " } },
        { "event_disaster_suffix", { " units.", " 个单位。" } },
        { "event_recession_prefix", { "A recession hit your finances hard: -$", "经济衰退重创了你的财务状况:-$" } },

        // First-build construction (see Business::constructionDaysRemaining /
        // Game::ConstructionInfo) -- the empty-plot signboard, the
        // construction-site countdown, and the Businesses overlay's
        // materials list + Start Construction button.
        { "construction_plot_sign_prefix",        { "Planned: ", "预定建造: " } },
        { "construction_site_days_left_prefix",   { "Under construction -- ", "施工中,还需 " } },
        { "construction_site_days_left_suffix",   { " day(s) left", " 天完工" } },
        { "construction_materials_header",        { "Materials needed:", "所需材料:" } },
        { "input_have_label",                     { "have: ", "现有: " } },
        { "start_construction_button",            { "Start Construction", "开始建造" } },
        { "construction_started_prefix",          { "Construction started on ", "已开始建造 " } },
        { "construction_completed_prefix",        { "Built: ", "已建成: " } },
        { "cancel_construction_button",           { "Cancel Construction", "取消建造" } },
        { "construction_cancelled_prefix",        { "Construction cancelled -- refunded $", "已取消建造,退回 $" } },
        { "commission_ship_button",               { "Commission Ship", "建造船只" } },
        { "ship_commissioned_prefix",              { "Ship commissioned! The Port can now sail to Fisher's Isle.", "船只已建成!现在可以从港口出航前往渔人岛了。" } },
        { "sail_button",                          { "Sail to Fisher's Isle", "出航前往渔人岛" } },
        { "arrived_at_isle",                      { "Arrived at Fisher's Isle.", "已抵达渔人岛。" } },
        { "construction_missing_materials",       { "Not enough materials:", "材料不足:" } },
        { "construction_in_progress_hint",        { "Already under construction -- just needs time.", "工地已在施工中,耐心等待完工即可。" } },

        // Overlay UI buttons
        { "close_button",   { "Close", "关闭" } },
        { "upgrade_button", { "Upgrade", "升级" } },
        { "legacy_button",  { "Legacy", "传承" } },
        { "deposit_button", { "Deposit", "存款" } },
        { "withdraw_button", { "Withdraw", "取款" } },
        { "buy_button",     { "Buy", "买入" } },
        { "sell_button",    { "Sell", "卖出" } },
        { "hire_button",    { "Hire", "雇佣" } },
        { "sleep_button",   { "Sleep", "睡觉" } },
        { "treat_button",   { "Treat", "治疗" } },
        { "qty_1",          { "1", "1" } },
        { "qty_10",         { "10", "10" } },
        { "qty_100",        { "100", "100" } },
        { "qty_all",        { "All", "全部" } },
        { "ff_15m",         { "15 min", "15分钟" } },
        { "ff_1h",          { "1 hour", "1小时" } },
        { "ff_4h",          { "4 hours", "4小时" } },
        { "ff_1d",          { "1 day", "1天" } },
        { "ff_1w",          { "1 week", "1周" } },
        { "selected_good_label", { "Selected: ", "已选择: " } },
        { "none_selected",  { "(click a good above)", "(点击上方的商品)" } },
        { "death_notice_title", { "You have passed away", "你已离世" } },
        { "death_notice_continue", { "Click to continue as the next generation", "点击以继续下一代" } },
        { "dialogue_continue_hint", { "Press E, click Close, or Esc to continue", "按 E 键、点击关闭,或按 Esc 继续" } },

        // NPC fetch quests
        { "quest_intro_prefix",    { "I need ", "我需要 " } },
        { "quest_reward_prefix",   { ". Bring it to me for $", ",给我的话给你 $" } },
        { "quest_reward_suffix",   { "!", "!" } },
        { "quest_turn_in_button",  { "Turn In", "交付" } },
        { "quest_need_prefix",     { "Need ", "还差 " } },
        { "quest_need_suffix",     { " more", " 个" } },
        { "quest_thanks",          { "Thank you so much!", "太感谢你了!" } },
        { "quest_completed_prefix", { "Quest complete: +$", "任务完成:+$" } },
        { "quest_not_enough_stock", { "You don't have enough of that good.\n", "你没有足够的该商品。\n" } },
        { "world_locked_hint", { "Not unlocked yet -- check the Production Tree at the Town Hall.", "还没解锁——去市政厅看看产业树。" } },

        // Pause menu (Escape from the walking-around world) and Settings
        { "pause_title",           { "Game Paused", "游戏已暂停" } },
        { "pause_save_prompt",     { "Save your progress?", "是否保存游戏进度？" } },
        { "pause_save_button",     { "Save", "保存" } },
        { "pause_saved_feedback",  { "Saved!", "已保存！" } },
        { "pause_resume_button",   { "Resume", "继续游戏" } },
        { "pause_settings_button", { "Settings", "设置" } },
        { "pause_quit_button",     { "Quit Game", "退出游戏" } },
        { "settings_title",            { "Settings", "设置" } },
        { "settings_back_button",      { "Back", "返回" } },
        { "settings_reset_button",     { "Reset to Defaults", "恢复默认" } },
        { "settings_reset_feedback",   { "Settings reset to defaults", "已恢复默认设置" } },
        { "settings_volume_label",     { "SFX Volume", "音效音量" } },
        { "settings_music_volume_label", { "Music Volume", "音乐音量" } },
        { "settings_resolution_label", { "Screen Size", "画面大小" } },
        { "settings_fullscreen_label", { "Fullscreen", "全屏" } },
        { "toggle_on",  { "On", "开" } },
        { "toggle_off", { "Off", "关" } },
        { "settings_keybinds_label",   { "Key Bindings", "按键绑定" } },
        { "settings_rebind_button",    { "Rebind", "重新绑定" } },
        { "settings_rebind_waiting",   { "Press a key... (Esc to cancel)", "请按下新按键...(Esc 取消)" } },
        { "settings_rebind_updated",   { "Key updated", "按键已更新" } },
        { "settings_rebind_swapped_prefix", { "Key swapped with ", "已与『" } },
        { "settings_rebind_swapped_suffix", { "", "』互换" } },
        { "key_move_up",       { "Move Up", "上" } },
        { "key_move_down",     { "Move Down", "下" } },
        { "key_move_left",     { "Move Left", "左" } },
        { "key_move_right",    { "Move Right", "右" } },
        { "key_interact",      { "Interact", "互动" } },
        { "key_quick_upgrade", { "Quick Upgrade", "快速升级" } },
        { "key_minimap",       { "Minimap", "小地图" } },
        { "key_minigame",      { "Minigame", "小游戏" } },
    };
}

const std::string& Localization::t(const std::string& key) {
    auto it = kTable.find(key);
    if (it == kTable.end()) return key;
    return current == Language::English ? it->second.first : it->second.second;
}
