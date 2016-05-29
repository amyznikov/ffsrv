/*
 * FFSRV database schema
 * 
 * Steps to setup ffdb database: 
 *   $ sudo su postgres
 *   $ psql template1
 *   template1=# CREATE DATABASE ffdb;
 *   template1=# CREATE USER ffsrv WITH PASSWORD 'ffsrv';
 *   template1=# GRANT ALL PRIVILEGES ON DATABASE ffdb TO ffsrv;
 *   template1=# \q
 *   $ exit
 *   $ psql -d ffdb -U ffsrv -f ffdb-pg.sql
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
    genpts integer,
    rtmo integer,
    itmo integer
) without oids;

comment on table  objects is 'All objects in a single table';
comment on column objects.name is 'Unique object name';
comment on column objects.type is 'Object type (see enum objtype)';
comment on column objects.source is 'The data source for this object: video source URL for inputs, or input name for outputs (transcoders)';
comment on column objects.opts is 'ffmpeg options to be applied to stream and/or codec contexts';
comment on column objects.re is 'Rate emulation stream index plus 1, or 0 if no rate emulation requested';
comment on column objects.genpts is 'Generate time-based PTS';
comment on column objects.rtmo is 'Read (recv) timeout in seconds';
comment on column objects.itmo is 'Idle timeout in seconds = auto shutdown non-referenced input after specified time';


/** 
 * Add input 
 */
drop function if exists add_input(name text, source text, opts text, re integer, genpts integer);
drop function if exists add_input(name text, source text, opts text, re integer, genpts integer, rtmo integer, itmo integer);
create or replace function add_input(name text, source text, opts text, re integer, genpts integer, rtmo integer, itmo integer) returns void
as $$
begin
    insert into objects (name, type, source, opts, re, genpts, rtmo, itmo) 
       values (name,'input', source, opts,re, genpts, rtmo, itmo);
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


/* some test 

select * from add_input('cam4', 'rtsp://cam4.sis.lan:554/live1.sdp', '-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0', 1, 0, 0, 0);
select * from add_input('cam5', 'rtsp://cam5.sis.lan:554/Streaming/Channels/2', '-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0', 1, 0, 0, 0);
select * from add_input('test', null, null, 1, 0, 0, 0);
select * from add_output('cam4/320x240', 'cam4', '-s 320x240 -c:v libx264');
select * from add_output('cam5/320x240', 'cam5', '-s 320x240 -c:v libx264');
 
*/

END;
