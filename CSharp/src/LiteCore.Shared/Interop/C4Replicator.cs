﻿//
// C4Replicator.cs
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using ObjCRuntime;

namespace LiteCore.Interop
{

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
        unsafe delegate void C4ReplicatorStateChangedCallback(C4Replicator* replicator,
            C4ReplicatorState replicatorState, void* context);


#if LITECORE_PACKAGED
    internal
#else

    public
#endif
        unsafe sealed class ReplicatorStateChangedCallback : IDisposable
    {
        private readonly object _context;
        private readonly Action<C4ReplicatorState, object> _callback;
        private long _id;
        private static long _NextID = 0L;

        private static readonly Dictionary<long, ReplicatorStateChangedCallback> _StaticMap =
            new Dictionary<long, ReplicatorStateChangedCallback>();

        internal C4ReplicatorStateChangedCallback NativeCallback { get; }

        internal void* NativeContext { get; }

        public ReplicatorStateChangedCallback(C4Replicator* replicator,
            Action<C4ReplicatorState, object> callback, object context)
        {
            var nextId = Interlocked.Increment(ref _NextID);
            _id = nextId;
            _StaticMap[_id] = this;
            NativeCallback = new C4ReplicatorStateChangedCallback(StateChanged);
            NativeContext = (void *)nextId;
        }

        [MonoPInvokeCallback(typeof(C4ReplicatorStateChangedCallback))]
        private static void StateChanged(C4Replicator* replicator, C4ReplicatorState state, void* context)
        {
            var id = (long)context;
            var obj = _StaticMap[id];
            obj._callback?.Invoke(state, obj._context);
        }

        public void Dispose()
        {
            _StaticMap.Remove(_id);
        }
    }


#if LITECORE_PACKAGED
    internal
#else
    public
#endif 
        unsafe static partial class Native
    {
        public static C4Replicator* c4repl_new(C4Database* db, C4Address remoteAddress, string remoteDatabaseName,
            C4ReplicatorMode push, C4ReplicatorMode pull, ReplicatorStateChangedCallback onStateChanged, C4Error* err)
        {
            return Native.c4repl_new(db, remoteAddress, remoteDatabaseName, push, pull, onStateChanged.NativeCallback,
                onStateChanged.NativeContext, err);
        }
    }
}
