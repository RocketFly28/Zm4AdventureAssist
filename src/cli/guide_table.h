/*
 * guide_table.h - 《生存大冒险》攻略表的题目 / 选项名称（英文）。
 *
 * 数据来源：生存大冒险攻略表.csv，按题号 101..505 的顺序（共 41 题 × 4 选项）。
 * 这里**只放名字**。成功要求 / 成功获得 / 失败获得一律由内核题库
 * （g_allQuestion / game_ctx_type_option）现算，保证显示与判定永远一致，
 * 攻略表里个别笔误不会污染界面。
 *
 * 下标：k_question_name[type][q_idx]、k_option_name[type][q_idx][opt]，
 *       type 为 0..4，q_idx 的范围见 totalQs[]。未使用的槽位为 NULL。
 */
#ifndef GUIDE_TABLE_H
#define GUIDE_TABLE_H

/* 题目名，下标 [type][q_idx] */
static const char *const k_question_name[5][15] = {
    {   /* type 1 */
        "Supply Run",
        "Mysterious Gift",
        "No Weapon",
        "Dangerous Task",
    },
    {   /* type 2 */
        "Xuanwu Gate",
        "Snow Lotus",
        "Dream Weaver Will",
        "Blue Water Rat",
        "Sky Thief",
        "Fallen Old Man",
        "Santa Claus",
        "Ocean Liner",
        "Changan City",
        "Guild Recruiting",
    },
    {   /* type 3 */
        "Peace Village",
        "Fierce Tiger Spirit",
        "Shadow on the Trail",
        "Four-Headed Worm",
        "Boy Alchemist",
        "Child Balloon",
        "Vermilion Street",
        "Guanyin Faction",
        "Brocade Peak",
        "Ruined Temple",
        "Boat of Time",
        "Eighteen Arhats",
        "Seven Fairies",
        "Taowu Cheat",
        "Old Man Meditating",
    },
    {   /* type 4 */
        "Glazed Palace",
        "Wounded Tang Monk",
        "Howling Dog",
        "Mirage Pavilion",
        "Old Man Chess",
        "Guanyin vs Tongtian",
        "Spiral Hell",
    },
    {   /* type 5 */
        "Metal Ancestor",
        "Wood Ancestor",
        "Water Ancestor",
        "Fire Ancestor",
        "Tongtian Lord",
    },
};

/* 选项名，下标 [type][q_idx][opt] */
static const char *const k_option_name[5][15][4] = {
    {   /* type 1 */
        { "Sell the Amulet", "Sell the Shoes", "Skip Shopping", "Borrow Money" },   /* Supply Run */
        { "Heaven Sword", "Peerless Manual", "Lucky Cat", "Little Alpaca" },   /* Mysterious Gift */
        { "Rob One Outside", "Borrow from Elder", "Grab a Stick", "Learn Smithing" },   /* No Weapon */
        { "Accept", "Refuse", "Stay Silent", "Name a Rival" },   /* Dangerous Task */
    },
    {   /* type 2 */
        { "Inspect Closely", "Carve a Name", "Take a Photo", "Walk On By" },   /* Xuanwu Gate */
        { "Meditate", "Pick It", "Mark the Spot", "Search Around" },   /* Snow Lotus */
        { "Snatch Snacks", "Submit a Story", "Beauty Contest", "Spoil the Plot" },   /* Dream Weaver Will */
        { "Kill It", "Chat with It", "Wait and See", "Follow It" },   /* Blue Water Rat */
        { "Join the Heist", "Track and Fight", "Set a Trap", "Report Him" },   /* Sky Thief */
        { "Beat Him", "Help Him Up", "Find a Witness", "Fake a Fall" },   /* Fallen Old Man */
        { "Steal the Sleigh", "Beat Him Up", "Ask for Gifts", "Slip Away" },   /* Santa Claus */
        { "Learn the Ropes", "Board Directly", "Invite a Friend", "Drill the Hull" },   /* Ocean Liner */
        { "Look Around", "Buy a Weapon", "Read the Notice", "Probe the Tunnel" },   /* Changan City */
        { "Stroll the Garden", "Spar (PvP)", "Guild Donation", "Promote the Guild" },   /* Guild Recruiting */
    },
    {   /* type 3 */
        { "Save the Child", "Stealth Rescue", "Strike a Chat", "Ignore Him" },   /* Peace Village */
        { "Fearless Charge", "Step Back", "Think It Through", "Make Friends" },   /* Fierce Tiger Spirit */
        { "Shout", "Punch Him", "Ask", "Turn and Flee" },   /* Shadow on the Trail */
        { "Help the Worm", "Help the Shadow", "Mediate", "Flee" },   /* Four-Headed Worm */
        { "Chase Him Off", "Help the Brew", "Jump in Furnace", "Kick the Furnace" },   /* Boy Alchemist */
        { "Do a Flip", "Fetch the Balloon", "Make a Face", "Look Away" },   /* Child Balloon */
        { "Charge In", "Get Help", "Hide", "Take a Detour" },   /* Vermilion Street */
        { "Azure Dragon", "White Tiger", "Vermilion Bird", "Black Tortoise" },   /* Guanyin Faction */
        { "The Tree is Odd", "Climb the Tree", "Fairy Hibiscus", "Talk to the Tree" },   /* Brocade Peak */
        { "Kick the Door", "Stay the Night", "Sense Evil", "Run First" },   /* Ruined Temple */
        { "Gift of Time", "Replay the Past", "Buy Items", "Observe" },   /* Boat of Time */
        { "Challenge", "Meditate", "Kneel", "Take as Master" },   /* Eighteen Arhats */
        { "Help the Fairies", "Tease Them", "Invite Me?", "Toss a Peach" },   /* Seven Fairies */
        { "Report", "Contact Leader", "Use the Cheat", "Sulk" },   /* Taowu Cheat */
        { "Meditate Too", "Punch Him", "Wake Him Up", "Do Nothing" },   /* Old Man Meditating */
    },
    {   /* type 4 */
        { "Scout the Path", "Push Forward", "No Way Here", "Three Hundred Rounds" },   /* Glazed Palace */
        { "Call a Doctor", "Find Disciples", "Sell Intel", "Drive Him Off" },   /* Wounded Tang Monk */
        { "Kick It", "Find Erlang", "Meat Bone", "Ignore It" },   /* Howling Dog */
        { "Fight the Demon", "Study It", "Wander Around", "Persuade Demon" },   /* Mirage Pavilion */
        { "Play Against Him", "Pat His Shoulder", "Find a Player", "Move a Piece" },   /* Old Man Chess */
        { "Help Guanyin", "Help Tongtian", "Offer a Bribe", "Stay Neutral" },   /* Guanyin vs Tongtian */
        { "Enter Boldly", "Leave Soon", "Study It", "Learn the Art" },   /* Spiral Hell */
    },
    {   /* type 5 */
        { "Charge In", "Make Friends", "Lure to Xuanwu", "Slip Away" },   /* Metal Ancestor */
        { "Burn It", "Fight", "Rebuild the Temple", "Eat a Peach" },   /* Wood Ancestor */
        { "Investigate", "Fight", "Shout Loudly", "Call Dragon Palace" },   /* Water Ancestor */
        { "Put Out the Fire", "Flee", "Watch from Afar", "Rush into Flames" },   /* Fire Ancestor */
        { "Refuse and Fight", "Take as Master", "Agree", "Refuse and Leave" },   /* Tongtian Lord */
    },
};

#endif /* GUIDE_TABLE_H */
