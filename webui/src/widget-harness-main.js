// THROWAWAY entry for manual/build verification of the widgets/* components.
// Deleted before task completion.
import { mount } from 'svelte';
import WidgetHarnessApp from './WidgetHarnessApp.svelte';
import './style.css';

mount(WidgetHarnessApp, { target: document.getElementById('app') });
