import os
import sys
import unittest
import json
import string
import random

wd = os.getcwd()
sys.path.append(wd+'/../python')

from pymargo.core import Engine
import pyyokan_common as yokan
from pyyokan_client import Client
from pyyokan_server import Provider

class TestStoreLoad(unittest.TestCase):

    def setUp(self):
        self.engine = Engine('tcp')
        self.mid = self.engine.get_internal_mid()
        self.addr = self.engine.addr()
        self.hg_addr = self.addr.get_internal_hg_addr()
        self.provider_id = 42
        self.provider = Provider(mid=self.mid,
                                 provider_id=self.provider_id,
                                 config='{"database":{"type":"map"}}')
        self.client = Client(mid=self.mid)
        self.db = self.client.make_database_handle(
            address=self.hg_addr,
            provider_id=self.provider_id)
        self.coll = self.db.create_collection(
            name="matt")
        self.reference = list()
        letters = string.ascii_letters
        for i in range(0,64):
            doc_len = random.randint(16, 128)
            doc = ''.join(random.choice(letters) for i in range(doc_len))
            self.reference.append(doc)
            self.coll.store(doc)

    def tearDown(self):
        del self.coll
        del self.db
        del self.addr
        del self.hg_addr
        del self.client
        del self.mid
        del self.provider
        del self.reference
        self.engine.finalize()

    def test_length(self):
        """Test that we can get the length of documents."""
        for i, doc in enumerate(self.reference):
            docsize = self.coll.length(id=i)
            self.assertEqual(docsize, len(doc))

        with self.assertRaises(yokan.Exception):
            self.coll.length(id=len(self.reference)+4)

    def test_length_multi(self):
        """Test that we can use length_multi."""
        ids = []
        expected = []
        for k, doc in enumerate(self.reference):
            ids.append(k)
            expected.append(doc)
        ids.append(len(self.reference)+4)
        expected.append(None)
        temp = list(zip(ids, expected))
        random.shuffle(temp)
        ids, expected = zip(*temp)

        docsizes = self.coll.length_multi(ids=ids)
        for i, (id, doc) in enumerate(zip(ids, expected)):
            if id >= len(self.reference):
                self.assertIsNone(docsizes[i])
            else:
                self.assertEqual(docsizes[i], len(doc))


if __name__ == '__main__':
    unittest.main()
