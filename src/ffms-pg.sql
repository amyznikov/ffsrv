/*
 * FFMS database schema
 * 
 * Steps to setup ffms database: 
 *   $ sudo su postgres
 *   $ psql template1
 *   template1=# CREATE DATABASE ffms;
 *   template1=# CREATE USER ffms WITH PASSWORD 'ffms';
 *   template1=# GRANT ALL PRIVILEGES ON DATABASE ffms TO ffms;
 *   template1=# \q
 *   $ exit
 *   $ psql -d ffms -U ffms -W ffms -f ffms.pg.sql
 */

BEGIN;



/** 
 * Object types 
 */
drop type if exists objtype  
	cascade;

create type objtype as enum (
    'input', 
    'output', 
    'mix', 
    'alias'
);

comment on type objtype is 
	'object types';


/** 
 * Objects 
 */

drop table if exists  objects
	cascade;

create table objects (
    name varchar(64) primary key,
    type objtype,
    source text,
    opts text,
    re integer,
    genpts integer
) without oids;




/** 
 * Add input 
 */
drop function if exists add_input(name text, source text, opts text, re integer, genpts integer);
create or replace function add_input(name text, source text, opts text, re integer, genpts integer) returns void
as $$
begin
    insert into objects (name, type, source, opts, re, genpts) 
       values (name,'input', source, opts,re, genpts);
end
$$ language plpgsql;


/** 
 * Add encoder 
 */
drop function if exists add_output(name text, source text, opts text);
create or replace function add_output(name text, source text, opts text) returns void
as $$
begin
    insert into objects (name, type, source, opts) 
       values (name, 'output', source, opts);
end
$$ language plpgsql;




/** 
 * Find object 
 */
drop function if exists find_object(name text);



/* some test */

select * from add_input('cam4', 'rtsp://cam4.sis.lan:554/live1.sdp', '-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0', 1, 0);
select * from add_input('cam5', 'rtsp://cam5.sis.lan:554/Streaming/Channels/2', '-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0', 1, 0);
select * from add_input('test', null, null, 1, 0);
select * from add_output('cam4/320x240', 'cam4', '-s 320x240 -c:v libx264');
select * from add_output('cam5/320x240', 'cam5', '-s 320x240 -c:v libx264');
 

END;
