/*
 * AllStatus.java
 *
 * Copyright (C) 2009-11 by RStudio, Inc.
 *
 * This program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
package org.rstudio.studio.client.common.vcs;

import com.google.gwt.core.client.JavaScriptObject;
import com.google.gwt.core.client.JsArray;

public class AllStatus extends JavaScriptObject
{
   protected AllStatus() {}

   public native final JsArray<StatusAndPath> getStatus() /*-{
      return this.status;
   }-*/;

   public native final BranchesInfo getBranches() /*-{
      return this.branches;
   }-*/;

   public native final boolean hasRemote() /*-{
      return this.has_remote;
   }-*/;
}
