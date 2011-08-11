package com.scalien.scaliendb;

import java.util.List;
import java.util.Collections;

public class StringKeyIterator implements java.lang.Iterable<String>, java.util.Iterator<String>
{
    private Table table;
    private String startKey;
    private String endKey;
    private String prefix;
    private int count;
    private int pos;
    private List<String> keys;
 
    public StringKeyIterator(Table table, StringRangeParams ps) throws SDBPException {
        this.table = table;
        this.startKey = ps.startKey;
        this.endKey = ps.endKey;
        this.prefix = ps.prefix;
        this.count = 100;
        
        query(false);
    }

    // for Iterable
    public java.util.Iterator<String> iterator() {
        return this;
    }
    
    // for Iterator
    public boolean hasNext() {
        try {
            if (pos == keys.size()) {
                if (keys.size() < count)
                    return false;
                startKey = keys.get(keys.size()-1);
                query(true);
            }
            if (keys.size() == 0)
                return false;
            return true;
        }
        catch(SDBPException e) {
            return false;
        }
    }
    
    // for Iterator
    public String next() {
        String e = keys.get(pos);
        pos++;
        return e;
    }
    
    // for Iterator
    public void remove() throws UnsupportedOperationException {
        throw new UnsupportedOperationException();
    }
    
    private void query(boolean skip) throws SDBPException {
        keys = table.getClient().listKeys(table.getTableID(), startKey, endKey, prefix, count, skip);
        Collections.sort(keys);
        pos = 0;
    }    
}