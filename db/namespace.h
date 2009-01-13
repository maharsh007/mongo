// namespace.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "../stdafx.h"

#include "jsobj.h"
#include "storage.h"

#include "../util/hashtab.h"
#include "../util/mmap.h"

class Cursor;

#pragma pack(push,1)

class Namespace {
public:
    enum { MaxNsLen = 128 };
    Namespace(const char *ns) {
        *this = ns;
    }
    Namespace& operator=(const char *ns) {
        memset(buf, 0, MaxNsLen); /* this is just to keep stuff clean in the files for easy dumping and reading */
        strcpy_s(buf, MaxNsLen, ns);
        return *this;
    }

    void kill() {
        buf[0] = 0x7f;
    }

    bool operator==(const char *r) {
        return strcmp(buf, r) == 0;
    }
    bool operator==(const Namespace& r) {
        return strcmp(buf, r.buf) == 0;
    }
    int hash() const {
        unsigned x = 0;
        const char *p = buf;
        while ( *p ) {
            x = x * 131 + *p;
            p++;
        }
        return (x & 0x7fffffff) | 0x8000000; // must be > 0
    }

    char buf[MaxNsLen];
};

const int Buckets = 19;
const int MaxBucket = 18;
const int MaxIndexes = 10;

class IndexDetails {
public:
    DiskLoc head; /* btree head */

    /* Location of index info object. Format:

         { name:"nameofindex", ns:"parentnsname", key: {keypattobject} }

       This object is in the system.indexes collection.  Note that since we
       have a pointer to the object here, the object in system.indexes must
       never move.
    */
    DiskLoc info;

    /* extract key value from the query object
       e.g., if key() == { x : 1 },
             { x : 70, y : 3 } -> { x : 70 }
       handles our embedded dot notation too.
    */
    BSONObj getKeyFromQuery(BSONObj& query) {
        BSONObj k = keyPattern();
        BSONObj res = query.extractFieldsUnDotted(k);
        assert(res.objsize() != 0); // guard against a seg fault if details is 0
        return res;
    }

    /* pull out the relevant key objects from obj, so we
       can index them.  Note that the set is multiple elements
       only when it's a "multikey" array.
       keys will be left empty if key not found in the object.
    */
    void getKeysFromObject( const BSONObj& obj, set<BSONObj>& keys) const;

    /* get the key pattern for this object.
       e.g., { lastname:1, firstname:1 }
    */
    BSONObj keyPattern() const {
        return info.obj().getObjectField("key");
    }

    // returns name of this index's storage area
    // database.table.$index
    string indexNamespace() {
        BSONObj io = info.obj();
        string s;
        s.reserve(Namespace::MaxNsLen);
        s = io.getStringField("ns");
        assert( !s.empty() );
        s += ".$";
        s += io.getStringField("name");
        return s;
    }

    string indexName() const { // e.g. "ts_1"
        BSONObj io = info.obj();
        return io.getStringField("name");
    }

    /* gets not our namespace name (indexNamespace for that),
       but the collection we index, its name.
       */
    string parentNS() const {
        BSONObj io = info.obj();
        return io.getStringField("ns");
    }

    /* delete this index.  does NOT celan up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
    */
    void kill();
};

extern int bucketSizes[];

/* this is the "header" for a collection that has all its details.  in the .ns file.
*/
class NamespaceDetails {
public:
    NamespaceDetails( const DiskLoc &loc, bool _capped ) {
        /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
        firstExtent = lastExtent = capExtent = loc;
        datasize = nrecords = 0;
        lastExtentSize = 0;
        nIndexes = 0;
        capped = _capped;
        max = 0x7fffffff;
        paddingFactor = 1.0;
        flags = 0;
        capFirstNewRecord = DiskLoc();
        // Signal that we are on first allocation iteration through extents.
        capFirstNewRecord.setInvalid();
        // For capped case, signal that we are doing initial extent allocation.
        if ( capped )
            deletedList[ 1 ].setInvalid();
        memset(reserved, 0, sizeof(reserved));
    }
    DiskLoc firstExtent;
    DiskLoc lastExtent;
    DiskLoc deletedList[Buckets];
    long long datasize;
    long long nrecords;
    int lastExtentSize;
    int nIndexes;
    IndexDetails indexes[MaxIndexes];
    int capped;
    int max; // max # of objects for a capped table.
    double paddingFactor; // 1.0 = no padding.
    int flags;
    DiskLoc capExtent;
    DiskLoc capFirstNewRecord;
    char reserved[108];

    enum {
        Flag_HaveIdIndex = 1 // set when we have _id index (ONLY if ensureIdIndex was called -- 0 if that has never been called)
    };

    /* you MUST call when adding an index.  see pdfile.cpp */
    void addingIndex(const char *thisns, IndexDetails& details);

    void aboutToDeleteAnIndex() {
        flags &= ~Flag_HaveIdIndex;
    }

    /* returns index of the first index in which the field is present. -1 if not present. */
    int fieldIsIndexed(const char *fieldName);

    void paddingFits() {
        double x = paddingFactor - 0.01;
        if ( x >= 1.0 )
            paddingFactor = x;
    }
    void paddingTooSmall() {
        double x = paddingFactor + 0.6;
        if ( x <= 2.0 )
            paddingFactor = x;
    }

    //returns offset in indexes[]
    int findIndexByName(const char *name) {
        for ( int i = 0; i < nIndexes; i++ ) {
            if ( strcmp(indexes[i].info.obj().getStringField("name"),name) == 0 )
                return i;
        }
        return -1;
    }

    /* return which "deleted bucket" for this size object */
    static int bucket(int n) {
        for ( int i = 0; i < Buckets; i++ )
            if ( bucketSizes[i] > n )
                return i;
        return Buckets-1;
    }

    /* allocate a new record.  lenToAlloc includes headers. */
    DiskLoc alloc(const char *ns, int lenToAlloc, DiskLoc& extentLoc);

    /* add a given record to the deleted chains for this NS */
    void addDeletedRec(DeletedRecord *d, DiskLoc dloc);

    void dumpDeleted(set<DiskLoc> *extents = 0);

    bool capLooped() const {
        return capped && capFirstNewRecord.isValid();        
    }
    
    // Start from firstExtent by default.
    DiskLoc firstRecord( const DiskLoc &startExtent = DiskLoc() ) const;

    // Start from lastExtent by default.
    DiskLoc lastRecord( const DiskLoc &startExtent = DiskLoc() ) const;

    bool inCapExtent( const DiskLoc &dl ) const;

    void checkMigrate();
    
private:
    Extent *theCapExtent() const { return capExtent.ext(); }
    void advanceCapExtent( const char *ns );
    void maybeComplain( const char *ns, int len ) const;
    DiskLoc __stdAlloc(int len);
    DiskLoc __capAlloc(int len);
    DiskLoc _alloc(const char *ns, int len);
    void compact();
    
    DiskLoc &firstDeletedInCapExtent();
    bool nextIsInCapExtent( const DiskLoc &dl ) const;
};

#pragma pack(pop)

/* these are things we know / compute about a namespace that are transient -- things
   we don't actually store in the .ns file.  so mainly caching of frequently used
   information.

   CAUTION: Are you maintaining this properly on a collection drop()?  A dropdatabase()?  Be careful.
            The current field "allIndexKeys" may have too many keys in it on such an occurrence;
            as currently used that does not cause anything terrible to happen.
*/
class NamespaceDetailsTransient : boost::noncopyable {
    string ns;
    bool haveIndexKeys;
    set<string> allIndexKeys;
    void computeIndexKeys();
public:
    NamespaceDetailsTransient(const char *_ns) : ns(_ns) {
        haveIndexKeys=false; /*lazy load them*/
    }

    /* get set of index keys for this namespace.  handy to quickly check if a given
       field is indexed (Note it might be a seconary component of a compound index.)
    */
    set<string>& indexKeys() {
        if ( !haveIndexKeys ) {
            haveIndexKeys=true;
            computeIndexKeys();
        }
        return allIndexKeys;
    }

    void addedIndex() {
        haveIndexKeys=false;
    }
private:
    static std::map<const char *,NamespaceDetailsTransient*> map;
public:
    static NamespaceDetailsTransient& get(const char *ns);
};

/* NamespaceIndex is the ".ns" file you see in the data directory.  It is the "system catalog"
   if you will: at least the core parts.  (Additional info in system.* collections.)
*/
class NamespaceIndex {
    friend class NamespaceCursor;
public:
    NamespaceIndex() { }

    /* returns true if we created (did not exist) during init() */
    bool init(const char *dir, const char *database);

    void add(const char *ns, DiskLoc& loc, bool capped) {
        Namespace n(ns);
        NamespaceDetails details( loc, capped );
        ht->put(n, details);
    }

    /* just for diagnostics */
    size_t detailsOffset(NamespaceDetails *d) {
        return ((char *) d) -  (char *) ht->nodes;
    }

    NamespaceDetails* details(const char *ns) {
        Namespace n(ns);
        NamespaceDetails *d = ht->get(n);
        if ( d )
            d->checkMigrate();
        return d;
    }

    void kill(const char *ns) {
        Namespace n(ns);
        ht->kill(n);
    }

    bool find(const char *ns, DiskLoc& loc) {
        NamespaceDetails *l = details(ns);
        if ( l ) {
            loc = l->firstExtent;
            return true;
        }
        return false;
    }

private:
    MemoryMappedFile f;
    HashTable<Namespace,NamespaceDetails> *ht;
};

extern const char *dbpath;

// "database.a.b.c" -> "database"
const int MaxClientLen = 256;
inline void nsToClient(const char *ns, char *database) {
    const char *p = ns;
    char *q = database;
    while ( *p != '.' ) {
        if ( *p == 0 )
            break;
        *q++ = *p++;
    }
    *q = 0;
    if (q-database>=MaxClientLen) {
        problem() << "nsToClient: ns too long. terminating, buf overrun condition" << endl;
        dbexit(60);
    }
}
inline string nsToClient(const char *ns) {
    char buf[MaxClientLen];
    nsToClient(ns, buf);
    return buf;
}
