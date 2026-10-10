-- 文件职责：
-- 清理历史上误发出的空邮件，避免 UE 邮箱里出现没有标题、没有正文、没有附件且无法正常操作的记录。
-- 调用流程：
-- 1. 先确认服务已经更新到“禁止发送空邮件”的版本。
-- 2. 在服务器执行：sudo mysql game_backend < db/mysql/004_cleanup_empty_mails.sql
-- 3. 玩家重新打开邮箱时，普通 GET /mail 不会再返回这些已删除空邮件。
--
-- 只清理满足以下条件的邮件：
-- - 标题为空或全是空白。
-- - 正文为空或全是空白。
-- - 附件数组为空或 rewards_json 无效。
-- - 当前还没有被逻辑删除。
--
-- 注意：
-- 这里是逻辑删除，不物理删除数据库行，方便后续需要时审计排查。

UPDATE mail_message
SET
    is_deleted = 1,
    deleted_at = COALESCE(deleted_at, CURRENT_TIMESTAMP),
    updated_at = CURRENT_TIMESTAMP
WHERE is_deleted = 0
  AND TRIM(COALESCE(title, '')) = ''
  AND TRIM(COALESCE(body_text, '')) = ''
  AND (
      JSON_VALID(rewards_json) = 0
      OR JSON_LENGTH(rewards_json) = 0
  );
