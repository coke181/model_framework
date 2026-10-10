-- 文件职责：
-- 修复历史上已经发出去的“有附件但正文为空”的奖励邮件。
-- 调用流程：
-- 1. 执行本文件后，UE 邮件详情不会再显示“这封邮件当前没有正文内容”。
-- 2. 这里只补正文，不删除邮件，也不改领取状态，避免玩家附件奖励丢失。
--
-- 背景：
-- 004_cleanup_empty_mails.sql 只清理“标题空 + 正文空 + 无附件”的真正空邮件。
-- 如果邮件有 gold/item/gem 等附件，直接删除可能造成奖励丢失，所以这里改成补默认正文。

UPDATE mail_message
SET
    body_text = '请领取下方附件奖励。感谢你的支持，祝你游戏愉快。',
    updated_at = CURRENT_TIMESTAMP
WHERE is_deleted = 0
  AND TRIM(COALESCE(body_text, '')) = ''
  AND JSON_VALID(rewards_json) = 1
  AND JSON_LENGTH(rewards_json) > 0;
