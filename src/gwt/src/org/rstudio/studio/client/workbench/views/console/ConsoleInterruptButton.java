/*
 * ConsoleInterruptButton.java
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
package org.rstudio.studio.client.workbench.views.console;

import com.google.gwt.dom.client.Style.Position;
import com.google.gwt.resources.client.ImageResource;
import com.google.gwt.user.client.Timer;
import com.google.gwt.user.client.ui.Composite;
import com.google.gwt.user.client.ui.SimplePanel;
import com.google.inject.Inject;
import org.rstudio.core.client.layout.FadeInAnimation;
import org.rstudio.core.client.widget.ToolbarButton;
import org.rstudio.studio.client.application.events.EventBus;
import org.rstudio.studio.client.workbench.commands.Commands;
import org.rstudio.studio.client.workbench.events.BusyEvent;
import org.rstudio.studio.client.workbench.events.BusyHandler;
import org.rstudio.studio.client.workbench.views.console.events.ConsolePromptEvent;
import org.rstudio.studio.client.workbench.views.console.events.ConsolePromptHandler;

public class ConsoleInterruptButton extends Composite
{
   @Inject
   public ConsoleInterruptButton(EventBus events,
                                 Commands commands)
   {
      // The SimplePanel wrapper is necessary for the toolbar button's "pushed"
      // effect to work.
      SimplePanel panel = new SimplePanel();
      panel.getElement().getStyle().setPosition(Position.RELATIVE);

      commands_ = commands;
      ImageResource icon = commands_.interruptR().getImageResource();
      ToolbarButton button = new ToolbarButton(icon,
                                               commands.interruptR());
      width_ = icon.getWidth() + 6;
      height_ = icon.getHeight();
      panel.setWidget(button);

      initWidget(panel);
      setVisible(false);

      events.addHandler(BusyEvent.TYPE, new BusyHandler()
      {
         public void onBusy(BusyEvent event)
         {
            if (event.isBusy())
               beginShow();
            else
               hide();
         }
      });

      /*
      JJ says:
      It is possible that the client could miss the busy = false event (if the
      client goes out of network coverage and then the server suspends before
      it can come back into coverage). For this reason I think that the icon's
      controller logic should subscribe to the ConsolePromptEvent and clear it
      whenenver a prompt occurs.
      */
      events.addHandler(ConsolePromptEvent.TYPE, new ConsolePromptHandler()
      {
         public void onConsolePrompt(ConsolePromptEvent event)
         {
            hide();
         }
      });
   }

   public int getWidth()
   {
      return width_;
   }

   public int getHeight()
   {
      return height_;
   }

   private void beginShow()
   {
      hide();

      commands_.interruptR().setEnabled(true);
      
      final Object nonce = new Object();
      nonce_ = nonce;
      new Timer()
      {
         @Override
         public void run()
         {
            if (nonce_ == nonce)
            {
               animation_ = new FadeInAnimation(
                     ConsoleInterruptButton.this, 1, null);
               animation_.run(250);
            }
         }
      }.schedule(750);
   }

   private void hide()
   {
      stopPending();
      setVisible(false);
      commands_.interruptR().setEnabled(false);
   }

   private void stopPending()
   {
      nonce_ = null;
      if (animation_ != null)
      {
         animation_.cancel();
         animation_ = null;
      }
   }

   private Object nonce_;
   private final int width_;
   private final int height_;
   private FadeInAnimation animation_;
   private final Commands commands_;
}
