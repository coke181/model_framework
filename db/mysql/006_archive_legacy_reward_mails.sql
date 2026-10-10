-- 文件职责：
-- 归档历史测试奖励邮件，解决 UE 邮箱里遗留的“显示有附件但领取/删除异常”的旧数据。
-- 调用流程：
-- 1. 先部署最新后端，后续新邮件会被严格校验，不会再生成空资源附件。
-- 2. 如确认这些 System 测试邮件不需要继续保留，执行：
--    sudo mysql game_backend < db/mysql/006_archive_legacy_reward_mails.sql
-- 3. 执行后普通 GET /mail 不会再返回这些历史测试邮件。
--
-- 归档范围：
-- - sender_name = 'System'
-- - title = '系统奖励邮件'
-- - 正文是系统默认补文案
-- - rewards_json 非空
-- - 当前未删除
--
-- 注意：
-- 这是逻辑删除，不物理删除数据库行。

UPDATE mail_message
SET
    is_deleted = 1,
    deleted_at = COALESCE(deleted_at, CURRENT_TIMESTAMP),
    updated_at = CURRENT_TIMESTAMP
WHERE is_deleted = 0
  AND sender_name = 'System'
  AND title = '系统奖励邮件'
  AND body_text = '请领取下方附件奖励。感谢你的支持，祝你游戏愉快。'
  AND JSON_VALID(rewards_json) = 1
  AND JSON_LENGTH(rewards_json) > 0;
