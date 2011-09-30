﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.IO;
using System.Runtime.Serialization.Json;

namespace Scalien
{
    class Utils
    {
        public static System.Random RandomNumber = new System.Random();

        public static byte[] JsonSerialize(object obj)
        {
            MemoryStream _stream = new MemoryStream();
            DataContractJsonSerializer jsonSerializer = new DataContractJsonSerializer(obj.GetType());
            _stream.SetLength(0);
            jsonSerializer.WriteObject(_stream, obj);
            return _stream.ToArray();
        }

        public static T JsonDeserialize<T>(byte[] data)
        {
            DataContractJsonSerializer jsonSerializer = new DataContractJsonSerializer(typeof(T));
            var stream = new MemoryStream(data);
            T t = (T)jsonSerializer.ReadObject(stream);
            return t;
        }

        public static string Id(Int64 num)
        {
            return String.Format("{0:0000000000000}", num);
        }

        public static void DeleteDBs(Client c)
        {
            foreach (Database db in c.GetDatabases())
                db.DeleteDatabase();
        }

        public static string RandomString(int size = 0)
        {
            string res = "";
            char ch;

            if (size == 0) size = RandomNumber.Next(5, 50);

            for (int i = 0; i < size; i++)
            {
                ch = Convert.ToChar(RandomNumber.Next(33, 126));
                res = res + ch;
            }
            return res;
        }

        // unique guarantees that ascii is unique in past ~35 minutes
        public static byte[] RandomASCII(int size = 0, bool unique = false)
        {
            byte[] res;
            Int64 mil;

            if (size == 0) size = RandomNumber.Next(5, 50);

            res = new byte[size];

            if (unique)
            {
                for (int i = 0; i < size; i++)
                    res[i] = (byte)RandomNumber.Next(0, 127);
            }
            else
            {
                for (int i = 0; i < size - 3; i++)
                    res[i] = (byte)RandomNumber.Next(0, 127);

                mil = System.DateTime.Now.Millisecond;
                res[size - 3] = (byte)System.Convert.ToSByte((mil / 16384) % 128);
                res[size - 2] = (byte)System.Convert.ToSByte((mil / 128) % 128);
                res[size - 1] = (byte)System.Convert.ToSByte(mil % 128);
            }

            return res;
        }

        public static byte[] ReadFile(string filePath)
        {
            byte[] buffer;
            FileStream fileStream = new FileStream(filePath, FileMode.Open, FileAccess.Read);
            try
            {
                int length = (int)fileStream.Length;  // get file length
                buffer = new byte[length];            // create buffer
                int count;                            // actual number of bytes read
                int sum = 0;                          // total number of bytes read

                // read until Read method returns 0 (end of the stream has been reached)
                while ((count = fileStream.Read(buffer, sum, length - sum)) > 0)
                    sum += count;  // sum is a buffer offset for next reading
            }
            finally
            {
                fileStream.Close();
            }
            return buffer;
        }

        public static bool ByteArraysEqual(byte[] a, byte[] b)
        {
            if (a.GetLength(0) != b.GetLength(0)) return false;

            for (int i = 0; i < a.GetLength(0); i++)
                if (a[i] != b[i]) return false;

            return true;
        }
    }
}