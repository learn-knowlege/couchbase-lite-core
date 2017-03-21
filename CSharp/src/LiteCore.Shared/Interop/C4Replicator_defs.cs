//
// C4Replicator_defs.cs
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
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

using LiteCore.Util;

namespace LiteCore.Interop
{
#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    enum C4ReplicatorMode
    {
        Disabled,
        Passive,
        OneShot,
        Continuous
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    enum C4ReplicatorActivityLevel
    {
        Stopped,
        Offline,
        Connecting,
        Idle,
        Busy
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe struct C4Replicator
    {
    }

#if LITECORE_PACKAGED
    internal
#else
    public
#endif
    unsafe struct C4ReplicatorState
    {
        public C4ReplicatorActivityLevel level;
        public C4Error error;
    }
}