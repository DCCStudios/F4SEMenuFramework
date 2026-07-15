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
	//
	// Its ONLY job:
	//   1. Add a single scrolling-list row labelled "F4SE Framework" to the
	//      pause menu, positioned directly above MCM's "Mod Configuration" row
	//      when the real MCM is installed (otherwise at the top of the list).
	//   2. When the player selects that row, ask the native code object
	//      (registered by the plugin on the host root as "f4semf") to open the
	//      already-existing ImGui overlay.
	//
	// It deliberately performs no rendering or UI of its own — the visible UI
	// is the plugin's ImGui overlay, drawn from the DLL via the D3D hook.
	public class F4SEFrameworkPause extends MovieClip
	{

		// Sentinel index for our injected list row. Distinct from vanilla pause
		// entries and from MCM's own row index (100) so the press handler can
		// positively identify a click on our row.
		private static const F4SE_ENTRY_INDEX:int = 500;

		// Label shown on the injected row.
		private static const F4SE_ENTRY_TEXT:String = "F4SE Framework";

		// MCM stores its row in entryList using this raw localisation key; we
		// match on it to sit directly above MCM's entry.
		private static const MCM_ENTRY_KEY:String = "$MOD_CONFIG";

		// Cached reference to the pause menu clip (root.Menu_mc) once found.
		private var menuClip:MovieClip = null;

		// True once our row has been spliced in; prevents duplicate insertion.
		private var injected:Boolean = false;

		// Frames spent waiting for MCM's row to appear before falling back to
		// inserting at the top (covers the "MCM not installed" case).
		private var framesWaited:int = 0;

		public function F4SEFrameworkPause()
		{
			super();
			// Injection is deferred to ENTER_FRAME because the pause menu's list
			// (and MCM's own row) are populated a frame or more after this SWF
			// finishes loading.
			addEventListener(Event.ENTER_FRAME, onEnterFrame);
		}

		private function onEnterFrame(e:Event):void
		{
			if (injected)
			{
				removeEventListener(Event.ENTER_FRAME, onEnterFrame);
				return;
			}

			// stage.getChildAt(0) is the host MainMenu.swf root; Menu_mc is the
			// actual menu clip. PauseMode is true only for the in-game pause
			// menu (not the title/main menu), matching MCM's own gate.
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

			// Wait until the scrolling list and its backing data array exist.
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

			var i:int;

			// If our row is already present, we are done (defensive).
			for (i = 0; i < entries.length; i++)
			{
				if (entries[i] && entries[i].index == F4SE_ENTRY_INDEX)
				{
					finishInjection();
					return;
				}
			}

			// Prefer to sit directly above MCM's row. MCM injects
			// asynchronously too, so give it a few frames to appear.
			var insertAt:int = -1;
			for (i = 0; i < entries.length; i++)
			{
				if (entries[i] && entries[i].text == MCM_ENTRY_KEY)
				{
					insertAt = i;
					break;
				}
			}

			framesWaited++;
			if (insertAt == -1)
			{
				// MCM row not present yet. Wait ~20 frames; if it never appears
				// (MCM not installed / disabled), drop our row at the top.
				if (framesWaited < 20)
				{
					return;
				}
				insertAt = 0;
			}

			// Insert immediately BEFORE the target index => visually above it.
			entries.splice(insertAt, 0, { "text": F4SE_ENTRY_TEXT, "index": F4SE_ENTRY_INDEX });
			list.InvalidateData();

			// Listen for list presses on the menu clip (events bubble up from
			// the list itself), exactly as MCM does.
			menuClip.addEventListener("BSScrollingList::itemPress", onItemPress);

			finishInjection();
		}

		private function finishInjection():void
		{
			injected = true;
			removeEventListener(Event.ENTER_FRAME, onEnterFrame);
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
