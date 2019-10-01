DROP ROLE IF EXISTS role1;
DROP ROLE IF EXISTS role2;
DROP ROLE IF EXISTS role3;

CREATE ROLE role1;
CREATE ROLE role2;
CREATE ROLE role3;

GRANT SELECT(name, age) ON mytable TO role1;
GRANT SELECT(address) ON mytable TO role1;

GRANT SELECT, INSERT, DROP ON mytable TO role2;

GRANT ALL PRIVILEGES ON mytable TO role3;
GRANT role1, role2 TO role3;

SELECT 'grants for role1:';
SHOW GRANTS FOR role1;
SELECT 'grants for role2:';
SHOW GRANTS FOR role2;
SELECT 'grants for role3:';
SHOW GRANTS FOR role3;

REVOKE SELECT(age) ON mytable FROM role1;
REVOKE INSERT ON mytable FROM role2;
REVOKE ALL ON mytable FROM role3;
REVOKE role1 FROM role3;

SELECT 'grants for role1 after revoke:';
SHOW GRANTS FOR role1;
SELECT 'grants for role2 after revoke:';
SHOW GRANTS FOR role2;
SELECT 'grants for role3 after revoke:';
SHOW GRANTS FOR role3;
