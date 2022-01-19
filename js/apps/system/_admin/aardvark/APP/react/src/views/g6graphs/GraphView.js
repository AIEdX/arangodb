import React from 'react';
import G6 from '@antv/g6';
import ReactDOM from 'react-dom';
import { Card } from 'antd';
import NodeStyleSelector from './NodeStyleSelector.js';
import styles from './graphview.module.css';

export class GraphView extends React.Component {

  constructor(props) {
    super(props)
    this.ref = React.createRef();
  }

  componentDidMount() {
    const toolbar = new G6.ToolBar({
      position: { x: 10, y: 10 },
    });
    const container = ReactDOM.findDOMNode(this.ref.current);
    console.log(`Size: ${container.offsetWidth} x ${container.offsetHeight}`);
    this.graph = new G6.Graph({
      container: this.ref.current,
      //width: container.offsetWidth,
      width: 1200,
      //height: container.offsetHeight,
      height: 400,
      plugins: [toolbar],
      enabledStack: true,
      layout: {
        type: 'gForce',
        minMovement: 0.01,
        maxIteration: 100,
        preventOverlap: true,
        damping: 0.99,
        fitView: true,
        linkDistance: 100
      },
      modes: {
        default: ['drag-canvas', 'zoom-canvas', 'drag-node'], // Allow users to drag canvas, zoom canvas, and drag nodes
      },
      defaultNode: {
        type: 'circle', // 'bubble'
        size: 30,
        labelCfg: {
          position: 'center',
          style: {
            fill: 'blue',
            fontStyle: 'bold',
            fontFamily: 'sans-serif',
            fontSize: 12
          },
        },
      },
    });

    this.graph.data(this.props.data);
    this.graph.render();
  }

  componentDidUpdate() {
    const container = ReactDOM.findDOMNode(this.ref.current);
    this.graph.changeSize(container.offsetWidth, container.offsetHeight);
    this.graph.data(this.props.data);
    this.graph.render();
  }

  getNodes = () => {
    const nodes = this.graph.getNodes();
    console.log("getNodes(): ", nodes);
  }

  getEdges = () => {
    const edges = this.graph.getEdges();
    console.log("getEdges(): ", edges);
  }

  addNode = () => {
    const nodeModel = {
      id: 'newnode',
      label: 'newnode',
      address: 'cq',
      x: 200,
      y: 150,
      style: {
        fill: 'white',
      },
    };
    
    this.graph.addItem('node', nodeModel);
  }

  addEdge = () => {
    const edgeModel = {
      source: 'newnode',
      target: '2',
      data: {
        type: 'name1',
        amount: '1000,00 元',
        date: '2022-01-13'
      }
    };
    
    this.graph.addItem('edge', edgeModel);
  }

  changeNodeStyle = (typeModel) => {
    this.graph.node((node) => {
      return {
        id: node.id,
        ...typeModel
      };
    });
    
    this.graph.data(this.props.data);
    this.graph.render();
  }

  render() {
    return <>
      <button onClick={this.getNodes}>Get nodes (new)</button>
      <button onClick={this.getEdges}>Get edges (new)</button>
      <button onClick={this.addNode}>Add node (new)</button>
      <button onClick={this.addEdge}>Add edge (new)</button>
      <NodeStyleSelector onNodeStyleChange={(typeModel) => this.changeNodeStyle(typeModel)} />
      <Card
          title="Pure JS G6 Graph"
        >
          <div ref={this.ref} className={styles.graphContainer}> </div>
      </Card>
    </>;
  }
}
