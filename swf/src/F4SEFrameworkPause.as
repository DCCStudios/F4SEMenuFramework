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
	//      at the top of the pause list (no MCM-relative placement).
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

		public function F4SEFrameworkPause()
		{
			super();
			// Same pattern as MCM_Main: one ENTER_FRAME once the host list
			// exists, then inject (or attach to a row the DLL already added).
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

			// Row may already exist (DLL injected it on PauseMenu open) —
			// still attach the press listener before we stop listening.
			if (!hasOurEntry(entries))
			{
				entries.splice(0, 0, { "text": F4SE_ENTRY_TEXT, "index": F4SE_ENTRY_INDEX });
				list.InvalidateData();
			}

			menuClip.addEventListener("BSScrollingList::itemPress", onItemPress);
			removeEventListener(Event.ENTER_FRAME, onEnterFrame);
		}

		private function hasOurEntry(entries:Array):Boolean
		{
			for (var i:int = 0; i < entries.length; i++)
			{
				if (entries[i] && entries[i].index == F4SE_ENTRY_INDEX)
				{
					return true;
				}
			}
			return false;
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
