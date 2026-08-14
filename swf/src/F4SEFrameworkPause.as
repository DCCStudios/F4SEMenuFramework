package
{
	import flash.display.MovieClip;
	import flash.events.Event;

	// Document (root) class for F4SEFramework.swf.
	//
	// This is a clean-room, self-contained SWF shipped with the F4SE Menu
	// Framework plugin. It shares NO code with MCM. The plugin's native
	// Scaleform callback (src/PauseMenuButton.cpp) loads this SWF into the
	// game's pause menu (Interface/MainMenu.swf) via a flash.display.Loader.
	// The DLL may also inject the list row from C++ on PauseMenu open so the
	// entry appears without waiting for this SWF to finish loading.
	//
	// Its job:
	//   1. Ensure a single scrolling-list row labelled "F4SE FRAMEWORK" exists
	//      at the user-configured slot of the pause list (read from
	//      `f4semf.buttonPos`; no MCM-relative placement). The DLL may insert
	//      the row early for instant appearance, but that runs before the game
	//      populates the list, so this class moves it to the right slot once
	//      the vanilla rows exist.
	//   2. When the player selects that row, call `f4semf.OpenMenu()` on the
	//      host root so the ImGui overlay opens.
	public class F4SEFrameworkPause extends MovieClip
	{

		// Sentinel index for our injected list row. Distinct from vanilla pause
		// entries and from MCM's own row index (100) so the press handler can
		// positively identify a click on our row.
		private static const F4SE_ENTRY_INDEX:int = 500;

		// Label shown on the injected row. ALL CAPS to match the vanilla pause
		// menu entries (MOD CONFIG, QUICKSAVE, ...), which are uppercased
		// localisation strings rather than styled by the list renderer.
		private static const F4SE_ENTRY_TEXT:String = "F4SE FRAMEWORK";

		// Cached reference to the pause menu clip (root.Menu_mc) once found.
		private var menuClip:MovieClip = null;

		// Press listener is attached once, as soon as the list exists, so a
		// DLL-injected row is clickable even while placement is still pending.
		private var listenerAttached:Boolean = false;

		// Frames spent waiting for the game to populate the vanilla rows.
		// Safety cap so we can't listen forever if the list stays empty.
		private var framesWaited:int = 0;
		private static const MAX_WAIT_FRAMES:int = 60;

		public function F4SEFrameworkPause()
		{
			super();
			// Same pattern as MCM_Main: act on ENTER_FRAME once the host list
			// exists AND the game has filled in the vanilla rows — placement
			// against a still-empty list would clamp every slot to the top,
			// which is exactly the bug the DLL-side injects have (they run
			// before population and always land at slot 0; this handler is
			// what moves the row to the user-configured slot).
			addEventListener(Event.ENTER_FRAME, onEnterFrame);
		}

		private function onEnterFrame(e:Event):void
		{
			if (!stage)
			{
				return;
			}

			var host:Object = stage.getChildAt(0);
			if (!host)
			{
				return;
			}

			var menu:MovieClip = host["Menu_mc"];
			if (!menu || !menu["PauseMode"])
			{
				return;
			}

			if (!menu["MainPanel_mc"] || !menu["MainPanel_mc"].List_mc)
			{
				return;
			}

			menuClip = menu;
			var list:Object = menu["MainPanel_mc"].List_mc;
			var entries:Array = list.entryList as Array;
			if (!entries)
			{
				return;
			}

			if (!listenerAttached)
			{
				menuClip.addEventListener("BSScrollingList::itemPress", onItemPress);
				listenerAttached = true;
			}

			var ourIdx:int = indexOfOurEntry(entries);
			var others:int = entries.length - (ourIdx >= 0 ? 1 : 0);

			// Vanilla rows not in yet — wait another frame (capped). The real
			// MCM relies on the same one-frame-later timing for iPosition:Main.
			framesWaited++;
			if (others == 0 && framesWaited < MAX_WAIT_FRAMES)
			{
				return;
			}

			// Configured slot from the DLL (0 = top, N = rows down, -1 =
			// bottom), clamped against the list without our own row.
			var pos:int = 0;
			var codeObj:Object = host["f4semf"];
			if (codeObj && codeObj.buttonPos !== undefined)
			{
				pos = int(codeObj.buttonPos);
			}
			var desired:int = (pos < 0) ? others : ((pos < others) ? pos : others);

			if (ourIdx != desired)
			{
				if (ourIdx >= 0)
				{
					entries.splice(ourIdx, 1);  // remove misplaced row first
				}
				entries.splice(desired, 0, { "text": F4SE_ENTRY_TEXT, "index": F4SE_ENTRY_INDEX });
				list.InvalidateData();
			}

			removeEventListener(Event.ENTER_FRAME, onEnterFrame);
		}

		private function indexOfOurEntry(entries:Array):int
		{
			for (var i:int = 0; i < entries.length; i++)
			{
				if (entries[i] && entries[i].index == F4SE_ENTRY_INDEX)
				{
					return i;
				}
			}
			return -1;
		}

		// Fired for every pause-menu list press. Acts only on our row.
		private function onItemPress(e:Event):void
		{
			if (!menuClip || !menuClip.MainPanel_mc || !menuClip.MainPanel_mc.List_mc)
			{
				return;
			}

			var sel:Object = menuClip.MainPanel_mc.List_mc.selectedEntry;
			if (!sel || sel.index != F4SE_ENTRY_INDEX)
			{
				return;
			}

			// Hand off to the native code object registered on the host root.
			// OpenMenu() opens the ImGui overlay ON TOP of the pause menu (the
			// menu stays open but the plugin blocks all its input while the
			// overlay is up; closing the overlay returns to this menu).
			if (!stage)
			{
				return;
			}

			var host:Object = stage.getChildAt(0);
			if (host && host["f4semf"])
			{
				host["f4semf"].OpenMenu();
			}
		}

	}

}
