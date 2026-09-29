import React from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './design-system/tokens.css';
import './styles.css';

const root = document.getElementById('root');
if (!root) throw new Error('Mock Studio root element is missing');

createRoot(root).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
