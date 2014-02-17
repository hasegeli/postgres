/* contrib/btree_gist/btree_gist--1.0--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION btree_gist UPDATE TO 1.1" to load this file. \quit

DROP OPERATOR FAMILY gist_inet_ops USING gist;

DROP OPERATOR FAMILY gist_cidr_ops USING gist;
