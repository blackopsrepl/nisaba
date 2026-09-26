# Changelog

All notable changes to this project will be documented in this file. See [commit-and-tag-version](https://github.com/absolute-version/commit-and-tag-version) for commit guidelines.

## 0.2.0 (2026-09-26)

### Features

* **btree:** persist the index as a paged B+tree snapshot 06ee8fd
* **cli:** add interactive and one-shot nisaba commands 077951b
* **codec:** encode records as self-describing TLV bodies 08303e6
* **db:** expose archive open, inscribe, canon, history, scan, retract 304419c
* **log:** append and recover framed records with crc validation 76c6e2b
* **model:** derive Canon from a supersession fold 0224f8f
* **skill:** teach coding agents to drive nisaba 58250af
* **util:** add buffer, varint, and crc32c primitives 2204296

### Bug Fixes

* **cli:** give every engine result an explicit exit code 85e0beb
* **readme:** replace the generic mascot f84bc45
* satisfy strict compile and tidy gates ahead of the lint target 8d652cc
