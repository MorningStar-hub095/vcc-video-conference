-- 迁移基线 0001：t_user 表与三条查询索引
--
-- 约定（改动前请读 deploy/migrate.sh 头部注释）：
--   1) 文件内容必须与 deploy/schema.sql 【逐字节一致】。
--      两份文件各自服务一条路径：schema.sql 给 docker-entrypoint-initdb.d
--      （全新容器首次初始化），本文件给 migrate.sh（已有库的演进）。
--      两条路径一旦内容分叉，就会出现"新装的库和升级上来的库结构不一致"，
--      这类问题极难排查，所以 migrate.sh 会在执行前用 md5 比对并拒绝分叉。
--   2) 每条迁移必须【幂等】：MySQL 的 DDL 会隐式提交，失败无法自动回滚，
--      重复执行不报错是唯一的安全网（建索引尤其如此：MySQL 8.0 没有
--      CREATE INDEX IF NOT EXISTS 语法）。
--   3) 只做加法，不做破坏性变更。要删列/改列请开新的一条迁移，并在
--      注释里写清楚"回滚该怎么做"。

CREATE DATABASE IF NOT EXISTS `vcc`
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_general_ci;

USE `vcc`;

CREATE TABLE IF NOT EXISTS `t_user` (
  `id`       INT AUTO_INCREMENT PRIMARY KEY,
  `tel`      VARCHAR(20)   NOT NULL DEFAULT '',
  `password` VARCHAR(64)   NOT NULL DEFAULT '',   -- bcrypt($2b$12$...) 固定 60 字符，64 足够
  `name`     VARCHAR(20)   NOT NULL DEFAULT '',
  `email`    VARCHAR(64)   NOT NULL DEFAULT '',
  `icon`     INT           NOT NULL DEFAULT 0,
  `feeling`  VARCHAR(100)  NOT NULL DEFAULT ''
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 登录/注册查重均按 tel / name / email 单列查询，建索引避免全表扫描
DROP PROCEDURE IF EXISTS `sp_add_index_if_absent`;
DELIMITER $$
CREATE PROCEDURE `sp_add_index_if_absent`(IN p_index VARCHAR(64), IN p_column VARCHAR(64))
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.statistics
        WHERE table_schema = DATABASE()
          AND table_name   = 't_user'
          AND index_name   = p_index
    ) THEN
        SET @ddl = CONCAT('CREATE INDEX `', p_index, '` ON `t_user` (`', p_column, '`)');
        PREPARE stmt FROM @ddl;
        EXECUTE stmt;
        DEALLOCATE PREPARE stmt;
    END IF;
END$$
DELIMITER ;

CALL `sp_add_index_if_absent`('idx_t_user_tel'  , 'tel'  );
CALL `sp_add_index_if_absent`('idx_t_user_name' , 'name' );
CALL `sp_add_index_if_absent`('idx_t_user_email', 'email');

DROP PROCEDURE IF EXISTS `sp_add_index_if_absent`;
