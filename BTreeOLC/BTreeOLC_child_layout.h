/*
 * BTreeOLC_child_layout.h - This file contains a modified version that
 *                           uses the key-value pair layout
 * 
 * We use this to test whether child node layout will affect performance
 */


#pragma once

#include <cassert>
#include <cstring>
#include <atomic>
#include <immintrin.h>
#include <sched.h>
// std::pair
#include <utility>

namespace btreeolc {
    std::atomic<long> total_restarts;
    std::atomic<long> inner_splits;
    std::atomic<long> leaf_splits;
    enum class PageType : uint8_t {
        BTreeInner = 1, BTreeLeaf = 2
    };

    static const uint64_t pageSize = 4 * 1024;

    struct OptLock {
        std::atomic<uint64_t> typeVersionLockObsolete{0b100};

        bool isLocked(uint64_t version) {
            return ((version & 0b10) == 0b10);
        }

        uint64_t readLockOrRestart(bool &needRestart) {
            uint64_t version;
            version = typeVersionLockObsolete.load();
            if (isLocked(version) || isObsolete(version)) {
                _mm_pause();
                needRestart = true;
            }
            return version;
        }

        void writeLockOrRestart(bool &needRestart) {
            uint64_t version;
            version = readLockOrRestart(needRestart);
            if (needRestart) return;

            upgradeToWriteLockOrRestart(version, needRestart);
            if (needRestart) return;
        }

        void upgradeToWriteLockOrRestart(uint64_t &version, bool &needRestart) {
            if (typeVersionLockObsolete.compare_exchange_strong(version, version + 0b10)) {
                version = version + 0b10;
            } else {
                _mm_pause();
                needRestart = true;
            }
        }

        void writeUnlock() {
            typeVersionLockObsolete.fetch_add(0b10);
        }

        bool isObsolete(uint64_t version) {
            return (version & 1) == 1;
        }

        void checkOrRestart(uint64_t startRead, bool &needRestart) const {
            readUnlockOrRestart(startRead, needRestart);
        }

        void readUnlockOrRestart(uint64_t startRead, bool &needRestart) const {
            needRestart = (startRead != typeVersionLockObsolete.load());
        }

        void writeUnlockObsolete() {
            typeVersionLockObsolete.fetch_add(0b11);
        }
    };

    struct NodeBase : public OptLock {
        PageType type;
        uint16_t count;
        pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    };

    struct BTreeLeafBase : public NodeBase {
        static const PageType typeMarker = PageType::BTreeLeaf;
    };

    template<class Key, class Payload>
    struct BTreeLeaf : public BTreeLeafBase {
        // This is the element type of the leaf node
        using KeyValueType = std::pair<Key, Payload>;
        static const uint64_t maxEntries = (pageSize - sizeof(NodeBase)) / (sizeof(KeyValueType));

        // This is the array that we perform search on
        KeyValueType data[maxEntries];

        BTreeLeaf() {
            count = 0;
            type = typeMarker;
        }

        bool isFull() { return count == maxEntries; };

        unsigned lowerBound(Key k) {
            unsigned lower = 0;
            unsigned upper = count;
            do {
                unsigned mid = ((upper - lower) / 2) + lower;
                // This is the key at the pivot position
                const Key &middle_key = data[mid].first;

                if (k < middle_key) {
                    upper = mid;
                } else if (k > middle_key) {
                    lower = mid + 1;
                } else {
                    return mid;
                }
            } while (lower < upper);
            return lower;
        }

        void insert(Key k, Payload p) {
            assert(count < maxEntries);
            if (count) {
                unsigned pos = lowerBound(k);
                if ((pos < count) && (data[pos].first == k)) {
                    // Upsert
                    data[pos].second = p;
                    return;
                }
                memmove(data + pos + 1, data + pos, sizeof(KeyValueType) * (count - pos));
                //memmove(payloads+pos+1,payloads+pos,sizeof(Payload)*(count-pos));
                data[pos].first = k;
                data[pos].second = p;
            } else {
                data[0].first = k;
                data[0].second = p;
            }
            count++;
        }

        BTreeLeaf *split(Key &sep) {
            leaf_splits++;
            BTreeLeaf *newLeaf = new BTreeLeaf();
            newLeaf->count = count - (count / 2);
            count = count - newLeaf->count;
            memcpy(newLeaf->data, data + count, sizeof(KeyValueType) * newLeaf->count);
            //memcpy(newLeaf->payloads, payloads+count, sizeof(Payload)*newLeaf->count);
            sep = data[count - 1].first;
            return newLeaf;
        }
    };

    struct BTreeInnerBase : public NodeBase {
        static const PageType typeMarker = PageType::BTreeInner;
    };

    template<class Key>
    struct BTreeInner : public BTreeInnerBase {
        static const uint64_t maxEntries = (pageSize - sizeof(NodeBase)) / (sizeof(Key) + sizeof(NodeBase *));
        NodeBase *children[maxEntries];
        Key keys[maxEntries];

        BTreeInner() {
            count = 0;
            type = typeMarker;
        }

        bool isFull() { return count == (maxEntries - 1); };

        unsigned lowerBoundBF(Key k) {
            auto base = keys;
            unsigned n = count;
            while (n > 1) {
                const unsigned half = n / 2;
                base = (base[half] < k) ? (base + half) : base;
                n -= half;
            }
            return (*base < k) + base - keys;
        }

        unsigned lowerBound(Key k) {
            unsigned lower = 0;
            unsigned upper = count;
            do {
                unsigned mid = ((upper - lower) / 2) + lower;
                if (k < keys[mid]) {
                    upper = mid;
                } else if (k > keys[mid]) {
                    lower = mid + 1;
                } else {
                    return mid;
                }
            } while (lower < upper);
            return lower;
        }

        BTreeInner *split(Key &sep) {
            inner_splits++;
            BTreeInner *newInner = new BTreeInner();
            newInner->count = count - (count / 2);
            count = count - newInner->count - 1;
            sep = keys[count];
            memcpy(newInner->keys, keys + count + 1, sizeof(Key) * (newInner->count + 1));
            memcpy(newInner->children, children + count + 1, sizeof(NodeBase *) * (newInner->count + 1));
            return newInner;
        }

        void insert(Key k, NodeBase *child) {
            assert(count < maxEntries - 1);
            unsigned pos = lowerBound(k);
            memmove(keys + pos + 1, keys + pos, sizeof(Key) * (count - pos + 1));
            memmove(children + pos + 1, children + pos, sizeof(NodeBase *) * (count - pos + 1));
            keys[pos] = k;
            children[pos] = child;
            std::swap(children[pos], children[pos + 1]);
            count++;
        }

    };


    template<class Key, class Value>
    struct BTree {
        std::atomic<NodeBase *> root;

        BTree() {
            root = new BTreeLeaf<Key, Value>();
        }

        void makeRoot(Key k, NodeBase *leftChild, NodeBase *rightChild) {
            auto inner = new BTreeInner<Key>();
            inner->count = 1;
            inner->keys[0] = k;
            inner->children[0] = leftChild;
            inner->children[1] = rightChild;
            root = inner;
        }

        void yield(int count) {
            if (count > 3)
                sched_yield();
            else
                _mm_pause();
        }

        long get_restarts() {
            return total_restarts;
        }

        long get_leaf_splits() {
            return leaf_splits;
        }

        long get_inner_splits() {
            return inner_splits;
        }

        void insert(Key k, Value v) {
            int restartCount = 0;
            restart:
            if (restartCount++)
                yield(restartCount);
            bool needRestart = false;

            // Current node
            NodeBase *node = root;
            uint64_t versionNode = node->readLockOrRestart(needRestart);
            if (needRestart || (node != root)) {
                total_restarts++;
                goto restart;
            }

            // Parent of current node
            BTreeInner<Key> *parent = nullptr;
            uint64_t versionParent;

            while (node->type == PageType::BTreeInner) {
                auto inner = static_cast<BTreeInner<Key> *>(node);

                // Split eagerly if full
                if (inner->isFull()) {
                    // Lock
                    if (parent) {
                        parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
                        if (needRestart) {
                            total_restarts++;
                            goto restart;
                        }
                    }

                    node->upgradeToWriteLockOrRestart(versionNode, needRestart);
                    if (needRestart) {
                        if (parent)
                            parent->writeUnlock();
                        total_restarts++;
                        goto restart;
                    }
                    if (!parent && (node != root)) { // there's a new parent
                        node->writeUnlock();
                        total_restarts++;
                        goto restart;
                    }
                    // Split
                    Key sep;
                    BTreeInner<Key> *newInner = inner->split(sep);
                    if (parent)
                        parent->insert(sep, newInner);
                    else
                        makeRoot(sep, inner, newInner);
                    // Unlock and restart
                    node->writeUnlock();
                    if (parent)
                        parent->writeUnlock();
                    total_restarts++;
                    goto restart;
                }

                if (parent) {
                    parent->readUnlockOrRestart(versionParent, needRestart);
                    if (needRestart) {
                        total_restarts++;
                        goto restart;
                    }
                }

                parent = inner;
                versionParent = versionNode;

                node = inner->children[inner->lowerBound(k)];
                inner->checkOrRestart(versionNode, needRestart);
                if (needRestart) {
                    total_restarts++;
                    goto restart;
                }
                versionNode = node->readLockOrRestart(needRestart);
                if (needRestart) {
                    total_restarts++;
                    goto restart;
                }
            }

            auto leaf = static_cast<BTreeLeaf<Key, Value> *>(node);
            pthread_mutex_lock(&node->lock);
            // Split leaf if full
            if (leaf->count == leaf->maxEntries) {
                // Lock
                if (parent) {
                    parent->upgradeToWriteLockOrRestart(versionParent, needRestart);
                    if (needRestart) {
                        total_restarts++;
                        pthread_mutex_unlock(&node->lock);
                        goto restart;
                    }
                }

//                node->upgradeToWriteLockOrRestart(versionNode, needRestart);
//                if (needRestart) {
//                    if (parent) parent->writeUnlock();
//                    total_restarts++;
//                    pthread_mutex_unlock(&node->lock);
//                    goto restart;
//                }
                if (!parent && (node != root)) { // there's a new parent
//                    node->writeUnlock();
                    total_restarts++;
                    pthread_mutex_unlock(&node->lock);
                    goto restart;
                }
                // Split
                Key sep;
                BTreeLeaf<Key, Value> *newLeaf = leaf->split(sep);
                if (parent)
                    parent->insert(sep, newLeaf);
                else
                    makeRoot(sep, leaf, newLeaf);
                // Unlock and restart
//                node->writeUnlock();
                pthread_mutex_unlock(&node->lock);
                if (parent)
                    parent->writeUnlock();
                total_restarts++;
                goto restart;
            } else {
                // only lock leaf node
//                pthread_mutex_lock(&node->lock);
//                node->upgradeToWriteLockOrRestart(versionNode, needRestart);
//                if (needRestart) {
//                    total_restarts++;
//                    pthread_mutex_unlock(&node->lock);
//                    goto restart;
//                }

                if (parent) {
                    parent->readUnlockOrRestart(versionParent, needRestart);
                    if (needRestart) {
//                        node->writeUnlock();
                        total_restarts++;
                        pthread_mutex_unlock(&node->lock);
                        goto restart;
                    }
                }
                leaf->insert(k, v);
//                node->writeUnlock();
                pthread_mutex_unlock(&node->lock);
                return; // success
            }
        }

        bool lookup(Key k, Value &result) {
            int restartCount = 0;
            restart:
            if (restartCount++)
                yield(restartCount);
            bool needRestart = false;

            NodeBase *node = root;
            uint64_t versionNode = node->readLockOrRestart(needRestart);
            if (needRestart || (node != root)) goto restart;

            // Parent of current node
            BTreeInner<Key> *parent = nullptr;
            uint64_t versionParent;

            while (node->type == PageType::BTreeInner) {
                auto inner = static_cast<BTreeInner<Key> *>(node);

                if (parent) {
                    parent->readUnlockOrRestart(versionParent, needRestart);
                    if (needRestart) goto restart;
                }

                parent = inner;
                versionParent = versionNode;

                node = inner->children[inner->lowerBound(k)];
                inner->checkOrRestart(versionNode, needRestart);
                if (needRestart) goto restart;
                versionNode = node->readLockOrRestart(needRestart);
                if (needRestart) goto restart;
            }

            BTreeLeaf<Key, Value> *leaf = static_cast<BTreeLeaf<Key, Value> *>(node);
            unsigned pos = leaf->lowerBound(k);
            bool success;
            if ((pos < leaf->count) && (leaf->data[pos].first == k)) {
                success = true;
                result = leaf->data[pos].second;
            }
            if (parent) {
                parent->readUnlockOrRestart(versionParent, needRestart);
                if (needRestart) goto restart;
            }
            node->readUnlockOrRestart(versionNode, needRestart);
            if (needRestart) goto restart;

            return success;
        }

        uint64_t scan(Key k, int range, Value *output) {
            int restartCount = 0;
            restart:
            if (restartCount++)
                yield(restartCount);
            bool needRestart = false;

            NodeBase *node = root;
            uint64_t versionNode = node->readLockOrRestart(needRestart);
            if (needRestart || (node != root)) goto restart;

            // Parent of current node
            BTreeInner<Key> *parent = nullptr;
            uint64_t versionParent;

            while (node->type == PageType::BTreeInner) {
                auto inner = static_cast<BTreeInner<Key> *>(node);

                if (parent) {
                    parent->readUnlockOrRestart(versionParent, needRestart);
                    if (needRestart) goto restart;
                }

                parent = inner;
                versionParent = versionNode;

                node = inner->children[inner->lowerBound(k)];
                inner->checkOrRestart(versionNode, needRestart);
                if (needRestart) goto restart;
                versionNode = node->readLockOrRestart(needRestart);
                if (needRestart) goto restart;
            }

            BTreeLeaf<Key, Value> *leaf = static_cast<BTreeLeaf<Key, Value> *>(node);
            unsigned pos = leaf->lowerBound(k);
            int count = 0;
            for (unsigned i = pos; i < leaf->count; i++) {
                if (count == range)
                    break;
                output[count++] = leaf->data[i].second;
            }

            if (parent) {
                parent->readUnlockOrRestart(versionParent, needRestart);
                if (needRestart) goto restart;
            }
            node->readUnlockOrRestart(versionNode, needRestart);
            if (needRestart) goto restart;

            return count;
        }


    };

}
