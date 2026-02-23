CREATE TABLE IF NOT EXISTS `mount_feeding` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'Character GUID',
    `satisfaction` INT UNSIGNED NOT NULL DEFAULT 999000,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-mount-feeding';
