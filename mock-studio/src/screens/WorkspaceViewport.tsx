import { useEffect, useRef } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

export function WorkspaceViewport() {
  const host = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const element = host.current;
    if (!element) return;
    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
    } catch {
      return; // Keep the CSS fallback on systems without WebGL.
    }
    renderer.setClearColor(0x101318);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.domElement.className = 'ws-viewport-canvas';
    element.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x101318);
    const camera = new THREE.PerspectiveCamera(50, 1, 0.1, 1000);
    camera.position.set(5, 2.8, 9);
    const cubeGeometry = new THREE.BoxGeometry(1.5, 1.5, 1.5);
    const cubeMaterial = new THREE.MeshStandardMaterial({ color: 0x318fbd, roughness: 0.9 });
    const cube = new THREE.Mesh(cubeGeometry, cubeMaterial);
    cube.position.y = 0.75;
    scene.add(cube);
    const grid = new THREE.GridHelper(200, 100, 0x48494a, 0x292c2e);
    scene.add(grid);
    const ambient = new THREE.AmbientLight(0xffffff, 2);
    const directional = new THREE.DirectionalLight(0xffffff, 2);
    directional.position.set(3, 8, 5);
    scene.add(ambient, directional);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.target.set(0, 0.8, 0);
    controls.enableDamping = true;
    controls.dampingFactor = 0.1;
    controls.update();
    const resize = () => {
      const { width, height } = element.getBoundingClientRect();
      if (width === 0 || height === 0) return;
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      renderer.setSize(width, height, false);
      renderer.render(scene, camera);
    };
    const observer = new ResizeObserver(resize);
    observer.observe(element);
    resize();
    let frame = 0;
    const animate = () => {
      frame = requestAnimationFrame(animate);
      if (controls.update()) renderer.render(scene, camera);
    };
    animate();
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      controls.dispose();
      cubeGeometry.dispose();
      cubeMaterial.dispose();
      grid.geometry.dispose();
      if (Array.isArray(grid.material)) grid.material.forEach(material => material.dispose());
      else grid.material.dispose();
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, []);

  return <div className="ws-viewport-scene" ref={host}><div className="ws-grid"><div className="ws-cube" /></div></div>;
}
